"""State vocabulary and session aggregation.

Pure logic, no I/O, no clock of its own — every function that cares about time takes
``now`` explicitly so tests can control it. See ADR-0004.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

# A session that has not reported for this long stops voting. Long enough that no
# plausible single tool call expires mid-work, short enough that a crashed session
# cannot wedge the light for the rest of the day. See ADR-0004.
DEFAULT_SESSION_TTL = 900.0  # seconds


class State(Enum):
    """The four states. Uppercase on the wire and in logs; see CONTEXT.md.

    Values are the aggregation priority: higher wins. ``UNKNOWN`` is deliberately
    absent — not knowing is a *link condition*, tracked on the device, not a state
    that can be aggregated or published (ADR-0007).
    """

    IDLE = 0
    BUSY = 1
    WAIT = 2
    ERROR = 3

    def __str__(self) -> str:
        return self.name

    @classmethod
    def parse(cls, text: str) -> State:
        """Parse a state name, case-insensitively.

        Raises:
            ValueError: with the full set of valid names, because this parses
                untrusted input from the wire and the CLI.
        """
        try:
            return cls[text.strip().upper()]
        except KeyError:
            valid = ", ".join(s.name for s in cls)
            raise ValueError(f"unknown state {text!r} (expected one of: {valid})") from None


@dataclass(frozen=True, slots=True)
class SessionState:
    """One session's vote, with the time it was cast."""

    state: State
    reason: str
    updated: float

    def is_live(self, now: float, ttl: float) -> bool:
        return (now - self.updated) < ttl


@dataclass(frozen=True, slots=True)
class Aggregate:
    """The displayed state: what the device should show right now."""

    state: State
    reason: str
    sessions: int

    def payload(self) -> dict[str, object]:
        """The ``wigwag/state`` message body.

        ``reason`` is diagnostic only — consumers switch on ``state`` and never on
        ``reason`` (CONTEXT.md).
        """
        return {"state": self.state.name, "reason": self.reason, "sessions": self.sessions}


class SessionStore:
    """Tracks per-session states and reduces them to one displayed state.

    The reduction is by priority — ``ERROR > WAIT > BUSY > IDLE`` — so a session
    that is merely working can never mask one that is blocked on the user. That
    masking is the specific failure this class exists to prevent (ADR-0004).
    """

    def __init__(self, ttl: float = DEFAULT_SESSION_TTL) -> None:
        if ttl <= 0:
            raise ValueError(f"ttl must be positive, got {ttl}")
        self._ttl = ttl
        self._sessions: dict[str, SessionState] = {}

    def set(self, session_id: str, state: State, reason: str, now: float) -> None:
        """Record a session's state. Also refreshes its TTL, which is the whole
        purpose of repeated ``BUSY`` heartbeats from the tool-use hooks."""
        if not session_id:
            raise ValueError("session_id must not be empty")
        self._sessions[session_id] = SessionState(state=state, reason=reason, updated=now)

    def drop(self, session_id: str) -> bool:
        """Remove a session, e.g. on ``SessionEnd``. Returns whether it existed.

        Idempotent: dropping an unknown session is not an error, because hooks can
        fire in orders we do not control.
        """
        return self._sessions.pop(session_id, None) is not None

    def expire(self, now: float) -> list[str]:
        """Drop sessions past their TTL. Returns the ids removed, for logging.

        Expiry is not a state change — an expired session simply stops voting.
        """
        dead = [sid for sid, s in self._sessions.items() if not s.is_live(now, self._ttl)]
        for sid in dead:
            del self._sessions[sid]
        return dead

    def live(self, now: float) -> dict[str, SessionState]:
        return {sid: s for sid, s in self._sessions.items() if s.is_live(now, self._ttl)}

    def aggregate(self, now: float) -> Aggregate:
        """Reduce all live sessions to the state the device should display.

        With no live sessions the answer is ``IDLE``: nothing is running, so nothing
        needs the user. That is a true statement, not a fallback.
        """
        live = self.live(now)
        if not live:
            return Aggregate(state=State.IDLE, reason="no-sessions", sessions=0)

        # max() on (priority, ...) picks the most urgent; ties break toward the most
        # recently updated so `reason` reflects what actually just happened.
        winner = max(live.values(), key=lambda s: (s.state.value, s.updated))
        return Aggregate(state=winner.state, reason=winner.reason, sessions=len(live))
