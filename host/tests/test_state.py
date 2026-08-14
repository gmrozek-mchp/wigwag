"""Aggregation and TTL — the guarantees ADR-0004 makes."""

from __future__ import annotations

import pytest

from wigwagd.state import Aggregate, SessionStore, State


def test_state_priority_ordering():
    assert State.ERROR.value > State.WAIT.value > State.BUSY.value > State.IDLE.value


@pytest.mark.parametrize("text", ["idle", "IDLE", " Busy ", "wait", "ErRoR"])
def test_parse_is_case_and_space_insensitive(text):
    assert isinstance(State.parse(text), State)


def test_parse_rejects_junk_and_names_the_valid_options():
    with pytest.raises(ValueError, match="IDLE, BUSY, WAIT, ERROR"):
        State.parse("UNKNOWN")


def test_no_sessions_aggregates_to_idle():
    store = SessionStore()
    agg = store.aggregate(now=100.0)
    assert agg == Aggregate(state=State.IDLE, reason="no-sessions", sessions=0)


def test_single_session_is_reported_verbatim():
    store = SessionStore()
    store.set("s1", State.BUSY, "UserPromptSubmit", now=10.0)
    agg = store.aggregate(now=11.0)
    assert (agg.state, agg.reason, agg.sessions) == (State.BUSY, "UserPromptSubmit", 1)


def test_wait_beats_busy_regardless_of_insertion_order():
    """The core promise: a working session must never mask a blocked one."""
    for order in ([("a", State.BUSY), ("b", State.WAIT)], [("a", State.WAIT), ("b", State.BUSY)]):
        store = SessionStore()
        for i, (sid, st) in enumerate(order):
            store.set(sid, st, "x", now=10.0 + i)
        assert store.aggregate(now=20.0).state is State.WAIT


def test_error_beats_everything():
    store = SessionStore()
    store.set("a", State.WAIT, "x", now=1.0)
    store.set("b", State.BUSY, "x", now=2.0)
    store.set("c", State.ERROR, "rate_limit", now=3.0)
    store.set("d", State.IDLE, "x", now=4.0)
    agg = store.aggregate(now=5.0)
    assert agg.state is State.ERROR
    assert agg.reason == "rate_limit"
    assert agg.sessions == 4


def test_busy_beats_idle():
    store = SessionStore()
    store.set("a", State.IDLE, "Stop", now=1.0)
    store.set("b", State.BUSY, "PreToolUse", now=2.0)
    assert store.aggregate(now=3.0).state is State.BUSY


def test_reason_comes_from_most_recent_among_equal_priority():
    store = SessionStore()
    store.set("a", State.BUSY, "older", now=1.0)
    store.set("b", State.BUSY, "newer", now=2.0)
    assert store.aggregate(now=3.0).reason == "newer"


def test_heartbeat_refreshes_ttl_without_changing_state():
    """Repeated BUSY from tool-use hooks exists to keep the session alive."""
    store = SessionStore(ttl=100.0)
    store.set("s1", State.BUSY, "PreToolUse", now=0.0)
    assert store.aggregate(now=99.0).sessions == 1

    store.set("s1", State.BUSY, "PostToolUse", now=99.0)  # heartbeat
    assert store.aggregate(now=150.0).sessions == 1, "heartbeat should have extended the TTL"
    assert store.aggregate(now=200.0).sessions == 0, "and only extended it, not removed it"


def test_expiry_prevents_a_crashed_session_wedging_the_light():
    """The failure ADR-0004 is built around: a session dies mid-WAIT without
    firing SessionEnd. It must stop voting on its own."""
    store = SessionStore(ttl=900.0)
    store.set("crashed", State.WAIT, "permission_prompt", now=0.0)
    assert store.aggregate(now=899.0).state is State.WAIT, "must hold WAIT inside the TTL"

    store.expire(now=901.0)
    assert store.aggregate(now=901.0).state is State.IDLE
    assert store.aggregate(now=901.0).sessions == 0


def test_expire_returns_the_ids_it_removed():
    store = SessionStore(ttl=10.0)
    store.set("old", State.BUSY, "x", now=0.0)
    store.set("fresh", State.BUSY, "x", now=100.0)
    assert store.expire(now=105.0) == ["old"]


def test_expired_session_does_not_mask_a_live_one():
    store = SessionStore(ttl=10.0)
    store.set("stale_wait", State.WAIT, "x", now=0.0)
    store.set("live_idle", State.IDLE, "Stop", now=100.0)
    # No explicit expire() call: aggregate must not count the dead session.
    assert store.aggregate(now=101.0).state is State.IDLE


def test_drop_is_idempotent_because_hook_order_is_not_guaranteed():
    store = SessionStore()
    store.set("s1", State.BUSY, "x", now=1.0)
    assert store.drop("s1") is True
    assert store.drop("s1") is False
    assert store.drop("never-existed") is False


def test_drop_removes_the_session_from_the_aggregate():
    store = SessionStore()
    store.set("s1", State.WAIT, "permission_prompt", now=1.0)
    store.drop("s1")
    assert store.aggregate(now=2.0).state is State.IDLE


def test_empty_session_id_is_rejected():
    with pytest.raises(ValueError, match="session_id"):
        SessionStore().set("", State.BUSY, "x", now=1.0)


def test_nonpositive_ttl_is_rejected():
    with pytest.raises(ValueError, match="ttl"):
        SessionStore(ttl=0)


def test_payload_shape_matches_the_documented_wire_format():
    store = SessionStore()
    store.set("s1", State.WAIT, "permission_prompt", now=1.0)
    assert store.aggregate(now=2.0).payload() == {
        "state": "WAIT",
        "reason": "permission_prompt",
        "sessions": 1,
    }
