"""The daemon: wires the listener, the session store and the publisher together.

Responsibilities, in order of importance:

1. Never lose an urgent transition (`WAIT`), because that is the light's whole job.
2. Never publish a state the aggregate did not actually reach — coalesce repeats.
3. Expire dead sessions so a crashed one cannot wedge the light (ADR-0004).
"""

from __future__ import annotations

import json
import logging
import threading
import time
from collections.abc import Callable

from .config import Config
from .protocol import Drop, Malformed, Ping, Set, parse
from .publisher import Publisher
from .state import Aggregate, SessionStore

log = logging.getLogger(__name__)

# How often to sweep for expired sessions. Well under the TTL, and cheap: it is a
# dict scan over a handful of entries.
_SWEEP_INTERVAL = 30.0


class Daemon:
    """Owns the session table and is the only publisher of state (CONTEXT.md)."""

    def __init__(
        self,
        config: Config,
        publisher: Publisher,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._cfg = config
        self._publisher = publisher
        self._clock = clock
        self._store = SessionStore(ttl=config.session_ttl)
        self._lock = threading.Lock()
        self._last_published: Aggregate | None = None

    # -- inbound ---------------------------------------------------------------

    def handle_datagram(self, data: bytes) -> None:
        """Apply one datagram, then republish if the aggregate changed."""
        msg = parse(data)
        now = self._clock()

        with self._lock:
            match msg:
                case Set(session_id=sid, state=state, reason=reason):
                    self._store.set(sid, state, reason, now)
                    log.debug("SET %s %s (%s)", sid, state, reason)
                case Drop(session_id=sid):
                    existed = self._store.drop(sid)
                    log.debug("DROP %s (existed=%s)", sid, existed)
                case Ping():
                    # Liveness probe only. Deliberately does not publish: a probe
                    # must not be able to cause broker traffic.
                    log.debug("PING")
                    return
                case Malformed(raw=raw, error=error):
                    # Log and drop. The sender is a hook and cannot be told.
                    log.warning("malformed datagram (%s): %r", error, raw[:120])
                    return

            self._publish_if_changed(now)

    # -- periodic --------------------------------------------------------------

    def sweep(self) -> list[str]:
        """Expire dead sessions and republish if that changed the aggregate."""
        now = self._clock()
        with self._lock:
            dead = self._store.expire(now)
            if dead:
                log.info("expired %d stale session(s): %s", len(dead), ", ".join(dead))
            self._publish_if_changed(now)
        return dead

    # -- outbound --------------------------------------------------------------

    @staticmethod
    def _publish_key(agg: Aggregate) -> tuple[object, ...]:
        """What counts as a change worth publishing.

        Deliberately excludes `reason`. `PreToolUse` and `PostToolUse` carry different
        reasons but the same state, so including it would republish the retained
        message twice per tool call — exactly the burst this coalescing exists to
        prevent. `reason` is diagnostic only and the device never switches on it
        (CONTEXT.md), so it rides along with whatever publish does happen.

        `sessions` *is* included: it changes rarely and is genuinely informative.
        """
        return (agg.state, agg.sessions)

    def _publish_if_changed(self, now: float) -> None:
        """Publish only real transitions.

        Caller must hold the lock. Tool-use hooks fire in bursts, so without this the
        daemon would republish an identical retained message dozens of times a second.
        """
        agg = self._store.aggregate(now)
        if self._last_published is not None and self._publish_key(agg) == self._publish_key(
            self._last_published
        ):
            return
        self._publisher.publish_state(self._cfg.topic("state"), agg.payload())
        log.info(
            "state %s (%s, %d session%s)",
            agg.state, agg.reason, agg.sessions, "" if agg.sessions == 1 else "s",
        )
        self._last_published = agg

    def publish_current(self) -> None:
        """Force a publish regardless of change, e.g. right after connecting."""
        now = self._clock()
        with self._lock:
            agg = self._store.aggregate(now)
            self._publisher.publish_state(self._cfg.topic("state"), agg.payload())
            self._last_published = agg

    # -- introspection ---------------------------------------------------------

    def _transport_label(self) -> str:
        """One line describing the live transport, for `wigwag status`."""
        if self._cfg.serial.enabled:
            return f"serial {self._cfg.serial.port}"
        b = self._cfg.broker
        return f"mqtt {b.host}:{b.port} ({'tls' if b.tls else 'plain'})"

    def snapshot(self) -> dict[str, object]:
        """Current state plus per-session detail, for `wigwag status`."""
        now = self._clock()
        with self._lock:
            agg = self._store.aggregate(now)
            sessions = {
                sid: {
                    "state": s.state.name,
                    "reason": s.reason,
                    "age_s": round(now - s.updated, 1),
                }
                for sid, s in self._store.live(now).items()
            }
        return {
            "aggregate": agg.payload(),
            "sessions": sessions,
            # What is *actually* carrying the state, not what is merely configured. Reporting a
            # broker while publishing over a wire is a small lie, and this is a status display.
            "transport": self._transport_label(),
            "config_source": self._cfg.source,
        }


class DaemonRunner:
    """Runs a `Daemon` with a listener and a sweep timer until stopped."""

    def __init__(self, daemon: Daemon, listener) -> None:  # noqa: ANN001
        self._daemon = daemon
        self._listener = listener
        self._stop = threading.Event()

    def run_forever(self) -> None:
        self._daemon.publish_current()
        while not self._stop.wait(_SWEEP_INTERVAL):
            self._daemon.sweep()

    def stop(self) -> None:
        self._stop.set()


def write_status_file(path, snapshot: dict[str, object]) -> None:  # noqa: ANN001
    """Write a status snapshot for `wigwag status` to read.

    The CLI cannot query the daemon over UDP (no reply path), and adding a request
    socket is more machinery than a status file for a single-user tool.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(snapshot, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)  # atomic, so a reader never sees a half-written file
