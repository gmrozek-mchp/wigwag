"""The hook→daemon wire protocol.

One line per UDP datagram, space-separated, ASCII. Deliberately trivial to emit: the
hook client is a few lines of bash with no `jq`, `sed`, `nc` or Python (ADR-0010), so
anything it has to produce must be formattable with `printf`.

    SET <STATE> <session_id> [reason]
    DROP <session_id>
    PING

Parsing is total: it never raises on malformed input, because the sender is a hook
that must never fail and cannot see our errors. Junk becomes a `Malformed` we log
and drop.
"""

from __future__ import annotations

from dataclasses import dataclass

from .state import State

MAX_DATAGRAM = 512
_MAX_REASON = 64


@dataclass(frozen=True, slots=True)
class Set:
    session_id: str
    state: State
    reason: str


@dataclass(frozen=True, slots=True)
class Drop:
    session_id: str


@dataclass(frozen=True, slots=True)
class Ping:
    pass


@dataclass(frozen=True, slots=True)
class Malformed:
    raw: str
    error: str


Message = Set | Drop | Ping | Malformed


def _clean(token: str) -> str:
    """Strip anything that would make a token unsafe to log or embed in JSON."""
    return "".join(c for c in token if c.isprintable())[:_MAX_REASON]


def parse(data: bytes) -> Message:
    """Parse one datagram. Never raises."""
    try:
        text = data.decode("utf-8", errors="replace").strip()
    except Exception as exc:  # pragma: no cover - decode with errors= cannot raise
        return Malformed(raw=repr(data[:64]), error=f"undecodable: {exc}")

    if not text:
        return Malformed(raw="", error="empty datagram")

    parts = text.split()
    verb = parts[0].upper()

    if verb == "PING":
        return Ping()

    if verb == "DROP":
        if len(parts) < 2:
            return Malformed(raw=text, error="DROP needs a session_id")
        return Drop(session_id=_clean(parts[1]))

    if verb == "SET":
        if len(parts) < 3:
            return Malformed(raw=text, error="SET needs a state and a session_id")
        try:
            state = State.parse(parts[1])
        except ValueError as exc:
            return Malformed(raw=text, error=str(exc))
        session_id = _clean(parts[2])
        if not session_id:
            return Malformed(raw=text, error="empty session_id")
        reason = _clean(parts[3]) if len(parts) > 3 else "unspecified"
        return Set(session_id=session_id, state=state, reason=reason)

    return Malformed(raw=text, error=f"unknown verb {verb!r}")
