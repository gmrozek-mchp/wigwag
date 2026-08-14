"""Wire protocol parsing.

The parser must be total: the sender is a hook that cannot see our errors and must
never be affected by them, so malformed input becomes data, never an exception.
"""

from __future__ import annotations

import pytest

from wigwagd.protocol import Drop, Malformed, Ping, Set, parse
from wigwagd.state import State


def test_set_minimal():
    msg = parse(b"SET BUSY abc123")
    assert msg == Set(session_id="abc123", state=State.BUSY, reason="unspecified")


def test_set_with_reason():
    msg = parse(b"SET WAIT abc123 Notification")
    assert msg == Set(session_id="abc123", state=State.WAIT, reason="Notification")


def test_trailing_newline_and_whitespace_are_tolerated():
    assert parse(b"  SET IDLE s1 Stop  \r\n") == Set("s1", State.IDLE, "Stop")


def test_verb_and_state_are_case_insensitive():
    assert parse(b"set busy s1") == Set("s1", State.BUSY, "unspecified")


def test_drop():
    assert parse(b"DROP abc123") == Drop(session_id="abc123")


def test_ping():
    assert parse(b"PING") == Ping()


@pytest.mark.parametrize(
    "raw",
    [
        b"",
        b"   ",
        b"SET",
        b"SET BUSY",
        b"DROP",
        b"NONSENSE s1",
        b"SET NOTASTATE s1",
        b"\x00\x01\x02",
    ],
)
def test_malformed_input_never_raises(raw):
    msg = parse(raw)
    assert isinstance(msg, Malformed), f"{raw!r} should be Malformed, got {msg}"
    assert msg.error, "Malformed must explain itself for the log"


def test_undecodable_bytes_become_malformed_not_an_exception():
    msg = parse(b"SET BUSY \xff\xfe\xfd")
    # Replacement characters are not printable-stripped to empty here, but whatever
    # happens it must be a value, not a raised exception.
    assert isinstance(msg, (Set, Malformed))


def test_unknown_state_names_the_valid_options_in_its_error():
    msg = parse(b"SET SLEEPING s1")
    assert isinstance(msg, Malformed)
    assert "IDLE" in msg.error and "ERROR" in msg.error


def test_control_characters_are_stripped_from_tokens():
    """Tokens end up in logs and JSON; they must not carry control characters."""
    msg = parse(b"SET BUSY s\x071 rea\x08son")
    assert isinstance(msg, Set)
    assert "\x07" not in msg.session_id
    assert "\x08" not in msg.reason


def test_long_reason_is_truncated_not_rejected():
    msg = parse(b"SET BUSY s1 " + b"x" * 500)
    assert isinstance(msg, Set)
    assert len(msg.reason) <= 64


def test_extra_tokens_beyond_reason_are_ignored():
    msg = parse(b"SET BUSY s1 reason and then some extra words")
    assert isinstance(msg, Set)
    assert msg.reason == "reason"


def test_realistic_session_id_survives_intact():
    sid = "0fec7bb8-66b2-4eb9-94fe-332c53b71bd1"
    msg = parse(f"SET WAIT {sid} Notification".encode())
    assert isinstance(msg, Set)
    assert msg.session_id == sid
