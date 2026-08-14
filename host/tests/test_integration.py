"""End-to-end over real loopback UDP, driving the actual `wg-notify` shell client.

This is the test that would catch a broken hook client, which unit tests cannot: it
executes the real script with real hook JSON on stdin and asserts the daemon's
aggregate changed. No broker involved — `NullPublisher` stands in.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import time
from pathlib import Path

import pytest

from wigwagd.config import Config
from wigwagd.daemon import Daemon
from wigwagd.listener import UdpListener
from wigwagd.publisher import NullPublisher

HOOK = Path(__file__).resolve().parent.parent / "hooks" / "wg-notify"

pytestmark = pytest.mark.skipif(
    not HOOK.is_file() or shutil.which("sh") is None,
    reason="needs the POSIX hook client and a POSIX shell",
)

SESSION = "0fec7bb8-66b2-4eb9-94fe-332c53b71bd1"


def hook_json(event: str, session_id: str = SESSION) -> str:
    """Realistic hook stdin: compact, single-line, session_id not first."""
    return json.dumps(
        {
            "cwd": "/Users/x/project",
            "session_id": session_id,
            "transcript_path": "/Users/x/.claude/t.jsonl",
            "hook_event_name": event,
            "permission_mode": "default",
        },
        separators=(",", ":"),
    )


@pytest.fixture
def rig():
    """A daemon behind a real UDP listener on an ephemeral port."""
    cfg = Config()
    cfg.listen_port = 0  # let the OS pick, so parallel runs cannot collide
    cfg.broker.resolve()
    pub = NullPublisher()
    daemon = Daemon(cfg, pub)
    listener = UdpListener(cfg.listen_host, 0, daemon.handle_datagram)
    listener.start()
    try:
        yield daemon, pub, listener
    finally:
        listener.stop()


def fire(listener: UdpListener, *args: str, event: str = "PreToolUse") -> subprocess.CompletedProcess:
    """Run the real hook client against the live listener."""
    proc = subprocess.run(
        ["sh", str(HOOK), *args],
        input=hook_json(event).encode(),
        capture_output=True,
        env={"WIGWAG_LISTEN_PORT": str(listener.port), "WIGWAG_HOST": "127.0.0.1", "PATH": "/usr/bin:/bin"},
        timeout=10,
    )
    time.sleep(0.08)  # let the listener thread drain the datagram
    return proc


def test_hook_client_is_silent_and_succeeds(rig):
    """CLAUDE.md Rule 3: exit 0, nothing on stdout, nothing on stderr."""
    _daemon, _pub, listener = rig
    proc = fire(listener, "SET", "BUSY", "PreToolUse")
    assert proc.returncode == 0
    assert proc.stdout == b"", f"stdout would be injected into the model's context: {proc.stdout!r}"
    assert proc.stderr == b"", f"stderr would be shown to the user: {proc.stderr!r}"


def test_state_reaches_the_daemon_with_the_session_id_intact(rig):
    daemon, pub, listener = rig
    fire(listener, "SET", "BUSY", "UserPromptSubmit", event="UserPromptSubmit")
    snap = daemon.snapshot()
    assert snap["aggregate"]["state"] == "BUSY"
    assert SESSION in snap["sessions"], f"session_id was not parsed: {snap['sessions']}"
    assert pub.published, "daemon should have published the transition"


def test_the_full_hook_sequence_over_the_wire(rig):
    """Phase 1 acceptance: IDLE -> BUSY -> WAIT -> IDLE, via the real client."""
    daemon, pub, listener = rig
    fire(listener, "SET", "IDLE", "SessionStart", event="SessionStart")
    fire(listener, "SET", "BUSY", "UserPromptSubmit", event="UserPromptSubmit")
    fire(listener, "SET", "BUSY", "PreToolUse")
    fire(listener, "SET", "WAIT", "Notification", event="Notification")
    assert daemon.snapshot()["aggregate"]["state"] == "WAIT"
    fire(listener, "SET", "IDLE", "Stop", event="Stop")
    assert daemon.snapshot()["aggregate"]["state"] == "IDLE"
    assert [p["state"] for _t, p in pub.published] == ["IDLE", "BUSY", "WAIT", "IDLE"]


def test_session_end_drops_the_session(rig):
    daemon, _pub, listener = rig
    fire(listener, "SET", "WAIT", "Notification", event="Notification")
    assert daemon.snapshot()["sessions"]
    fire(listener, "DROP", event="SessionEnd")
    snap = daemon.snapshot()
    assert snap["sessions"] == {}
    assert snap["aggregate"]["state"] == "IDLE"


def test_two_sessions_wait_beats_busy_over_the_wire(rig):
    daemon, _pub, listener = rig
    subprocess.run(
        ["sh", str(HOOK), "SET", "BUSY", "PreToolUse"],
        input=hook_json("PreToolUse", "session-aaa").encode(),
        capture_output=True,
        env={"WIGWAG_LISTEN_PORT": str(listener.port), "PATH": "/usr/bin:/bin"},
        timeout=10,
    )
    subprocess.run(
        ["sh", str(HOOK), "SET", "WAIT", "Notification"],
        input=hook_json("Notification", "session-bbb").encode(),
        capture_output=True,
        env={"WIGWAG_LISTEN_PORT": str(listener.port), "PATH": "/usr/bin:/bin"},
        timeout=10,
    )
    time.sleep(0.1)
    snap = daemon.snapshot()
    assert snap["aggregate"]["state"] == "WAIT"
    assert snap["aggregate"]["sessions"] == 2


def test_client_succeeds_silently_when_the_daemon_is_down(rig):
    """Rule 3's hardest case: nothing listening at all."""
    _daemon, _pub, listener = rig
    proc = subprocess.run(
        ["sh", str(HOOK), "SET", "BUSY", "PreToolUse"],
        input=hook_json("PreToolUse").encode(),
        capture_output=True,
        env={"WIGWAG_LISTEN_PORT": "9", "PATH": "/usr/bin:/bin"},  # discard port
        timeout=10,
    )
    assert proc.returncode == 0
    assert proc.stdout == b""
    assert proc.stderr == b""


def test_malformed_stdin_still_exits_clean(rig):
    """Junk on stdin must not make the hook fail; session_id degrades to 'unknown'."""
    _daemon, _pub, listener = rig
    for payload in (b"", b"not json at all", b"{}", b'{"session_id":}'):
        proc = subprocess.run(
            ["sh", str(HOOK), "SET", "BUSY", "PreToolUse"],
            input=payload,
            capture_output=True,
            env={"WIGWAG_LISTEN_PORT": str(listener.port), "PATH": "/usr/bin:/bin"},
            timeout=10,
        )
        assert proc.returncode == 0, f"failed on {payload!r}"
        assert proc.stdout == b"" and proc.stderr == b""


def test_unknown_verb_is_ignored_without_error(rig):
    daemon, pub, listener = rig
    proc = fire(listener, "FROBNICATE")
    assert proc.returncode == 0
    assert pub.published == []
    assert daemon.snapshot()["sessions"] == {}


def test_hook_client_latency_is_within_budget(rig):
    """Rule 3 requires this stay cheap; it runs on every tool call."""
    _daemon, _pub, listener = rig
    env = {"WIGWAG_LISTEN_PORT": str(listener.port), "PATH": "/usr/bin:/bin"}
    payload = hook_json("PreToolUse").encode()
    timings = []
    for _ in range(15):
        t = time.perf_counter()
        subprocess.run(["sh", str(HOOK), "SET", "BUSY", "PreToolUse"],
                       input=payload, capture_output=True, env=env, timeout=10)
        timings.append((time.perf_counter() - t) * 1000)
    timings.sort()
    median = timings[len(timings) // 2]
    # Generous ceiling: measured ~3 ms, and CI machines are slower. This is a
    # regression guard against accidentally adding a Python or node dependency,
    # which would put it in the tens of milliseconds.
    assert median < 50, f"hook client median {median:.1f} ms is too slow for the hook path"
