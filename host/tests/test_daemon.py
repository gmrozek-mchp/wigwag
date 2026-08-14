"""Daemon behaviour: coalescing, and the full hook sequence end to end.

Uses `NullPublisher` so there is no broker and no `paho-mqtt` dependency.
"""

from __future__ import annotations

import pytest

from wigwagd.config import Config
from wigwagd.daemon import Daemon
from wigwagd.publisher import NullPublisher


class FakeClock:
    """Controllable monotonic clock, so TTL tests need no sleeping."""

    def __init__(self, t: float = 1000.0) -> None:
        self.t = t

    def __call__(self) -> float:
        return self.t

    def advance(self, dt: float) -> None:
        self.t += dt


@pytest.fixture
def rig():
    cfg = Config()
    cfg.broker.resolve()
    pub = NullPublisher()
    clock = FakeClock()
    return Daemon(cfg, pub, clock=clock), pub, clock


def states(pub: NullPublisher) -> list[str]:
    return [p["state"] for _t, p in pub.published]


def test_first_publish_is_the_initial_idle(rig):
    daemon, pub, _ = rig
    daemon.publish_current()
    assert states(pub) == ["IDLE"]


def test_publishes_on_transition(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET BUSY s1 UserPromptSubmit")
    assert states(pub) == ["BUSY"]


def test_repeated_identical_state_is_coalesced(rig):
    """Tool-use hooks fire in bursts; the broker must not see 20 identical
    retained messages a second."""
    daemon, pub, _ = rig
    for _ in range(20):
        daemon.handle_datagram(b"SET BUSY s1 PreToolUse")
    assert states(pub) == ["BUSY"], "only the transition should publish"


def test_reason_change_alone_does_not_republish(rig):
    """PreToolUse and PostToolUse differ only in reason. If that republished, every
    tool call would push two retained messages — the exact burst coalescing exists
    to stop."""
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET BUSY s1 PreToolUse")
    assert len(pub.published) == 1
    for _ in range(10):
        daemon.handle_datagram(b"SET BUSY s1 PostToolUse")
        daemon.handle_datagram(b"SET BUSY s1 PreToolUse")
    assert len(pub.published) == 1, "reason churn must not reach the broker"


def test_topic_is_prefixed_from_config(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET BUSY s1 x")
    topic, _payload = pub.published[-1]
    assert topic == "wigwag/state"


def test_malformed_datagram_publishes_nothing_and_does_not_raise(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"complete nonsense")
    daemon.handle_datagram(b"")
    daemon.handle_datagram(b"SET NOTASTATE s1")
    assert pub.published == []


def test_full_hook_sequence_idle_busy_wait_idle(rig):
    """The Phase 1 acceptance path from docs/PLAN.md."""
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET IDLE s1 SessionStart")
    daemon.handle_datagram(b"SET BUSY s1 UserPromptSubmit")
    daemon.handle_datagram(b"SET BUSY s1 PreToolUse")   # coalesced
    daemon.handle_datagram(b"SET WAIT s1 Notification")
    daemon.handle_datagram(b"SET IDLE s1 Stop")
    assert states(pub) == ["IDLE", "BUSY", "WAIT", "IDLE"]


def test_two_sessions_wait_wins_over_busy(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET BUSY s1 PreToolUse")
    daemon.handle_datagram(b"SET WAIT s2 Notification")
    assert states(pub) == ["BUSY", "WAIT"]
    _topic, payload = pub.published[-1]
    assert payload["sessions"] == 2


def test_busy_session_continuing_does_not_clear_a_waiting_one(rig):
    """The masking bug ADR-0004 exists to prevent, at the daemon level.

    The session count changes when s2 appears, so a republish is expected — but the
    state must never leave WAIT.
    """
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET WAIT s1 Notification")
    for _ in range(5):
        daemon.handle_datagram(b"SET BUSY s2 PreToolUse")
    assert set(states(pub)) == {"WAIT"}, "WAIT must survive another session's heartbeats"
    assert pub.published[-1][1]["sessions"] == 2


def test_session_end_drops_and_republishes(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET WAIT s1 Notification")
    daemon.handle_datagram(b"DROP s1")
    assert states(pub) == ["WAIT", "IDLE"]


def test_dropping_one_of_two_falls_back_to_the_other(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"SET BUSY s1 PreToolUse")
    daemon.handle_datagram(b"SET WAIT s2 Notification")
    daemon.handle_datagram(b"DROP s2")
    assert states(pub) == ["BUSY", "WAIT", "BUSY"]


def test_sweep_expires_a_crashed_session_and_republishes(rig):
    daemon, pub, clock = rig
    daemon.handle_datagram(b"SET WAIT s1 permission_prompt")
    assert states(pub) == ["WAIT"]

    clock.advance(901.0)  # past the 900 s default TTL
    # A crashed session never sent DROP; only the sweep can rescue the light.
    dead = daemon.sweep()
    assert dead == ["s1"]
    assert states(pub) == ["WAIT", "IDLE"]


def test_sweep_with_nothing_expired_publishes_nothing(rig):
    daemon, pub, clock = rig
    daemon.handle_datagram(b"SET BUSY s1 x")
    clock.advance(10.0)
    assert daemon.sweep() == []
    assert len(pub.published) == 1


def test_ping_is_accepted_and_changes_nothing(rig):
    daemon, pub, _ = rig
    daemon.handle_datagram(b"PING")
    assert pub.published == []


def test_snapshot_reports_sessions_and_broker(rig):
    daemon, _pub, clock = rig
    daemon.handle_datagram(b"SET WAIT s1 Notification")
    clock.advance(5.0)
    snap = daemon.snapshot()
    assert snap["aggregate"]["state"] == "WAIT"
    assert snap["sessions"]["s1"]["state"] == "WAIT"
    assert snap["sessions"]["s1"]["age_s"] == 5.0
    assert snap["broker"] == "localhost:1883"
    assert snap["tls"] is False


def test_snapshot_omits_expired_sessions(rig):
    daemon, _pub, clock = rig
    daemon.handle_datagram(b"SET WAIT s1 x")
    clock.advance(1000.0)
    assert daemon.snapshot()["sessions"] == {}
