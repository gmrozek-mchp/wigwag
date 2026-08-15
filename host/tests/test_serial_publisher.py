"""The wired transport (ADR-0018, ADR-0020).

No hardware and no ``pyserial``: a stub module is injected into ``sys.modules``, which is exactly
what the lazy import inside ``SerialPublisher.start`` makes possible. The behaviour worth pinning
down is the failure handling — a status light must never be able to take the daemon down with it —
and the heartbeat, because the device stops trusting us after ten seconds of silence (D111).
"""

from __future__ import annotations

import sys
import types

import pytest

from wigwagd.config import Config
from wigwagd.publisher import SerialPublisher, discover_serial_port


class FakePort:
    """Enough of pyserial's Serial to see what the daemon says, and to make it fail on demand."""

    def __init__(self, name, baud, timeout=None, write_timeout=None):
        self.name = name
        self.baud = baud
        self.written: list[str] = []
        self.closed = False
        self.fail_writes = False
        self.pending = b""

    def write(self, data: bytes) -> int:
        if self.fail_writes:
            raise OSError("device went away")
        self.written.extend(data.decode().splitlines())
        return len(data)

    @property
    def in_waiting(self) -> int:
        return len(self.pending)

    def read(self, n: int) -> bytes:
        out, self.pending = self.pending[:n], self.pending[n:]
        return out

    def close(self) -> None:
        self.closed = True


@pytest.fixture
def fake_serial(monkeypatch):
    """Install a stub ``serial`` module and hand back the ports it opens."""
    opened: list[FakePort] = []

    def _open(name, baud, timeout=None, write_timeout=None):
        p = FakePort(name, baud, timeout, write_timeout)
        opened.append(p)
        return p

    mod = types.ModuleType("serial")
    mod.Serial = _open
    monkeypatch.setitem(sys.modules, "serial", mod)
    return opened


def _cfg(port="/dev/ttyFAKE"):
    cfg = Config()
    cfg.serial.port = port
    return cfg


def test_serial_is_off_by_default():
    """MQTT stays the default. Setting a port is what opts in."""
    assert not Config().serial.enabled
    assert _cfg().serial.enabled


def test_start_silences_echo_then_announces(fake_serial):
    pub = SerialPublisher(_cfg())
    pub.start()

    port = fake_serial[0]
    # Echo first: we are a program, and echo would double every byte on a shared wire.
    assert port.written == ["echo off", "host on"]
    assert port.baud == 115200


def test_state_is_sent_as_a_bare_word(fake_serial):
    """The console vocabulary, not MQTT's JSON: the device parses one enum (CONTEXT.md)."""
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].written.clear()

    pub.publish_state("wigwag/state", {"state": "BUSY", "reason": "PostToolUse", "sessions": 2})

    # reason and sessions are deliberately dropped -- the device never read them.
    assert fake_serial[0].written == ["state BUSY"]


def test_a_payload_with_no_state_sends_nothing(fake_serial):
    """Better to send nothing and log than to invent a state (Rule 4)."""
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].written.clear()

    pub.publish_state("wigwag/state", {"reason": "nonsense"})
    pub.publish_state("wigwag/state", {"state": ""})

    assert fake_serial[0].written == []


def test_unknown_topics_are_skipped_not_guessed(fake_serial):
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].written.clear()

    pub.publish_state("wigwag/button", {"event": "press"})

    assert fake_serial[0].written == []


def test_heartbeat_repeats_host_on(fake_serial):
    """Five of these fit inside the device's 10 s trust window (D111)."""
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].written.clear()

    pub.heartbeat()
    pub.heartbeat()

    assert fake_serial[0].written == ["host on", "host on"]


def test_stop_says_goodbye(fake_serial):
    """Which the device honours at once, rather than waiting out its timeout (D112)."""
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].written.clear()

    pub.stop()

    assert fake_serial[0].written == ["host off"]
    assert fake_serial[0].closed


def test_a_dead_device_does_not_kill_the_daemon(fake_serial):
    """The whole point: unplugging a desk toy must never raise into the daemon's loop."""
    pub = SerialPublisher(_cfg())
    pub.start()
    first = fake_serial[0]
    first.fail_writes = True

    # None of these may raise.
    pub.publish_state("wigwag/state", {"state": "ERROR"})
    pub.heartbeat()
    pub.stop()

    assert first.closed, "a failed write closes the port so it can be reopened"


def test_heartbeat_reopens_a_lost_port(fake_serial):
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].fail_writes = True
    pub.heartbeat()          # fails, closes
    assert len(fake_serial) == 1

    pub.heartbeat()          # reopens
    assert len(fake_serial) == 2, "the next heartbeat reopens the port"
    assert fake_serial[1].written == ["echo off", "host on", "host on"]


def test_missing_port_is_retried_quietly(monkeypatch):
    """An unplugged device is a normal condition, not a crash and not a log every 2 s."""
    def _explode(*_a, **_k):
        raise OSError("no such device")

    mod = types.ModuleType("serial")
    mod.Serial = _explode
    monkeypatch.setitem(sys.modules, "serial", mod)

    pub = SerialPublisher(_cfg())
    with pytest.raises(OSError):
        pub.start()          # start is allowed to fail loudly: the user asked for this port

    pub.heartbeat()          # but the loop must not
    pub.heartbeat()


def test_device_output_is_drained_and_survives_garbage(fake_serial):
    """The device narrates real things; reading them is the only way a host ever sees them."""
    pub = SerialPublisher(_cfg())
    pub.start()
    fake_serial[0].pending = b"wigwag: RESET BY WATCHDOG\r\n\xff\xfe binary\r\n"

    pub.heartbeat()          # must not raise on undecodable bytes

    assert fake_serial[0].pending == b""


def test_discovery_matches_the_datasheet_identity(monkeypatch):
    """VID 0x04D8 / PID 0x00DD, the MCP2221A's factory USB identity."""
    Port = types.SimpleNamespace

    def _comports():
        return [
            Port(device="/dev/ttyOTHER", vid=0x0403, pid=0x6001),
            Port(device="/dev/ttyWIGWAG", vid=0x04D8, pid=0x00DD),
        ]

    tools = types.ModuleType("serial.tools")
    lp = types.ModuleType("serial.tools.list_ports")
    lp.comports = _comports
    tools.list_ports = lp
    monkeypatch.setitem(sys.modules, "serial", types.ModuleType("serial"))
    monkeypatch.setitem(sys.modules, "serial.tools", tools)
    monkeypatch.setitem(sys.modules, "serial.tools.list_ports", lp)

    assert discover_serial_port() == "/dev/ttyWIGWAG"


@pytest.mark.parametrize(
    "ports",
    [
        [],
        [
            types.SimpleNamespace(device="/dev/a", vid=0x04D8, pid=0x00DD),
            types.SimpleNamespace(device="/dev/b", vid=0x04D8, pid=0x00DD),
        ],
    ],
    ids=["none", "several"],
)
def test_discovery_refuses_to_guess(monkeypatch, ports):
    """Zero or many is an error. Picking the first of two would eventually drive a 3D printer."""
    lp = types.ModuleType("serial.tools.list_ports")
    lp.comports = lambda: ports
    tools = types.ModuleType("serial.tools")
    tools.list_ports = lp
    monkeypatch.setitem(sys.modules, "serial", types.ModuleType("serial"))
    monkeypatch.setitem(sys.modules, "serial.tools", tools)
    monkeypatch.setitem(sys.modules, "serial.tools.list_ports", lp)

    with pytest.raises(RuntimeError):
        discover_serial_port()


def test_auto_asks_for_discovery(fake_serial, monkeypatch):
    monkeypatch.setattr("wigwagd.publisher.discover_serial_port", lambda: "/dev/ttyFOUND")

    pub = SerialPublisher(_cfg("auto"))
    pub.start()

    assert fake_serial[0].name == "/dev/ttyFOUND"


def test_env_and_validation():
    import os

    os.environ["WIGWAG_SERIAL_PORT"] = "/dev/ttyENV"
    os.environ["WIGWAG_SERIAL_BAUD"] = "9600"
    try:
        cfg = Config.load()
        assert cfg.serial.port == "/dev/ttyENV"
        assert cfg.serial.baud == 9600
        assert cfg.serial.enabled
    finally:
        del os.environ["WIGWAG_SERIAL_PORT"]
        del os.environ["WIGWAG_SERIAL_BAUD"]


def test_mqtt_heartbeat_is_a_deliberate_noop():
    """Retention plus the Last Will already speak for us; periodic chatter would prove nothing."""
    from wigwagd.publisher import MqttPublisher

    MqttPublisher(Config()).heartbeat()   # must not raise, must not need a client
