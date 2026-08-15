"""Publishing, behind an interface so the daemon is testable without a broker or a device.

Two real transports, chosen by configuration and never both at once (ADR-0018): MQTT to a
broker, or lines on a serial port to a directly attached device. `paho-mqtt` and `pyserial`
are both imported lazily inside their publishers, so the pure logic, the protocol parser and
the whole test suite run with no third-party dependency installed at all (D31, ADR-0020).
"""

from __future__ import annotations

import json
import logging
from typing import Protocol

log = logging.getLogger(__name__)

_PYSERIAL_HINT = (
    "pyserial is not installed. It is only needed for the wired (serial) transport:\n"
    "    python3 -m pip install 'wigwagd[serial]'\n"
    "or leave serial.port unset to publish over MQTT instead."
)


class Publisher(Protocol):
    """What the daemon needs from a transport."""

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        """Publish the displayed state, **retained**.

        Retention is the point (ADR-0003): the broker replays this to the device the
        instant it subscribes, so the light is correct immediately after a reboot or
        a dropped link, with no host involvement.
        """
        ...

    def heartbeat(self) -> None:
        """Say "still here", if this transport needs telling.

        The asymmetry lives here rather than in the daemon's main loop. Over MQTT the answer is
        nothing: `host_online` is retained and the Last Will reports a death, so the broker
        speaks for us even when we are silent. A serial line has neither, so the device demands
        a repeated `host on` and treats silence as loss of trust (D111, CONTEXT.md).
        """
        ...

    def start(self) -> None: ...

    def stop(self) -> None: ...


class NullPublisher:
    """Records instead of publishing. Used by tests and by ``--dry-run``."""

    def __init__(self) -> None:
        self.published: list[tuple[str, dict[str, object]]] = []
        self.heartbeats = 0
        self.started = False

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        self.published.append((topic, payload))
        log.info("would publish %s %s", topic, json.dumps(payload, sort_keys=True))

    def heartbeat(self) -> None:
        self.heartbeats += 1

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.started = False


class MqttPublisher:
    """Real MQTT via paho, supporting a local or remote broker with optional TLS."""

    def __init__(self, cfg) -> None:  # noqa: ANN001 - avoids importing config here
        self._cfg = cfg
        self._client = None

    def start(self) -> None:
        try:
            import paho.mqtt.client as mqtt
        except ModuleNotFoundError as exc:
            raise RuntimeError(
                "paho-mqtt is not installed. Install it with:\n"
                "    python3 -m pip install -r host/requirements.txt\n"
                "or run the daemon with --dry-run to exercise everything except publishing."
            ) from exc

        b = self._cfg.broker
        # CallbackAPIVersion is required by paho 2.x; guard so paho 1.x also works.
        if hasattr(mqtt, "CallbackAPIVersion"):
            client = mqtt.Client(
                mqtt.CallbackAPIVersion.VERSION2, client_id=b.client_id
            )
        else:  # pragma: no cover - paho 1.x fallback
            client = mqtt.Client(client_id=b.client_id)

        if b.username:
            client.username_pw_set(b.username, b.password or None)

        if b.tls:
            client.tls_set(ca_certs=b.ca_cert or None)
            if b.insecure_skip_verify:
                client.tls_insecure_set(True)

        # Last Will: if this daemon dies, say so rather than leave a stale story.
        client.will_set(
            self._cfg.topic("host_online"), payload="0", qos=1, retain=True
        )

        client.on_connect = self._on_connect
        client.on_disconnect = self._on_disconnect

        log.info(
            "connecting to broker %s:%s (tls=%s, auth=%s)",
            b.host, b.port, b.tls, bool(b.username),
        )
        client.connect_async(b.host, b.port, keepalive=b.keepalive)
        client.loop_start()
        self._client = client

    def _on_connect(self, client, _userdata, _flags, reason_code, _props=None) -> None:
        if getattr(reason_code, "is_failure", False) or (
            isinstance(reason_code, int) and reason_code != 0
        ):
            log.error("broker refused connection: %s", reason_code)
            return
        log.info("connected to broker")
        client.publish(self._cfg.topic("host_online"), "1", qos=1, retain=True)

    def _on_disconnect(self, _client, _userdata, *args) -> None:
        # paho retries on its own; log so an unstable link is visible rather than silent.
        log.warning("disconnected from broker (%s); paho will retry", args[-1] if args else "?")

    def heartbeat(self) -> None:
        """Nothing to do, and that is the interesting part.

        `host_online` is published once, retained, with `0` registered as the Last Will. The
        broker therefore holds our liveness for any subscriber that arrives later and announces
        our death on our behalf, so periodic chatter would add load and prove nothing extra.
        """

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        if self._client is None:
            raise RuntimeError("publisher not started")
        body = json.dumps(payload, separators=(",", ":"), sort_keys=True)
        self._client.publish(topic, body, qos=1, retain=True)
        log.debug("published %s %s", topic, body)

    def stop(self) -> None:
        if self._client is None:
            return
        # Publish a clean offline marker so shutdown is distinguishable from a crash.
        try:
            self._client.publish(self._cfg.topic("host_online"), "0", qos=1, retain=True)
            self._client.loop_stop()
            self._client.disconnect()
        finally:
            self._client = None


def discover_serial_port() -> str:
    """Find a wigwag by the MCP2221A's factory USB identity.

    VID 0x04D8 / PID 0x00DD, the power-on defaults of the USBVID and USBPID chip-settings
    registers (MCP2221A datasheet, Registers 1-5 to 1-8). Both are user-programmable, so a
    device with a customised identity must be named explicitly.

    Raises rather than guessing when the answer is not exactly one. A daemon that silently
    picked the first of two serial ports would eventually drive somebody's 3D printer.
    """
    try:
        from serial.tools import list_ports
    except ModuleNotFoundError as exc:  # pragma: no cover - depends on the environment
        raise RuntimeError(_PYSERIAL_HINT) from exc

    matches = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x04D8, 0x00DD)]

    if not matches:
        raise RuntimeError(
            "no MCP2221A found (looked for USB 04d8:00dd). Name the port explicitly with "
            "WIGWAG_SERIAL_PORT=/dev/... if the device uses a customised USB identity."
        )
    if len(matches) > 1:
        raise RuntimeError(
            f"found {len(matches)} MCP2221A devices ({', '.join(matches)}); "
            "name one explicitly with WIGWAG_SERIAL_PORT"
        )

    log.info("discovered wigwag on %s", matches[0])
    return matches[0]


class SerialPublisher:
    """The wired transport: command lines on the device's console UART.

    Renders the same four states as the MQTT path in the console's own vocabulary (CONTEXT.md):
    a bare word rather than JSON, because the device parses one enum and a 64 KB part does not
    need a JSON reader to do it. `reason` and `sessions` are dropped — the device never read
    them; they exist for humans watching MQTT.

    Every write is guarded. The device may be unplugged at any moment, and a status light must
    never be able to take the daemon down with it: failures are logged, the port is closed, and
    the next heartbeat tries to reopen it.
    """

    def __init__(self, cfg) -> None:  # noqa: ANN001 - avoids importing config here
        self._cfg = cfg
        self._port = None
        self._warned = False

    def _open(self) -> None:
        try:
            import serial
        except ModuleNotFoundError as exc:
            raise RuntimeError(_PYSERIAL_HINT) from exc

        name = self._cfg.serial.port
        if name.strip().lower() == "auto":
            name = discover_serial_port()

        # timeout=0 so reads never block the daemon's loop; write_timeout so a wedged port
        # cannot either. Both matter: this runs on the same thread as the session sweep.
        self._port = serial.Serial(name, self._cfg.serial.baud, timeout=0, write_timeout=2)
        log.info("serial transport open on %s at %d", name, self._cfg.serial.baud)

        # We are a program, not a person: echo would double every byte on the wire.
        self._send("echo off")
        self._send("host on")

    def _send(self, line: str) -> bool:
        if self._port is None:
            return False
        try:
            self._port.write((line + "\r\n").encode("ascii", "replace"))
            log.debug("serial > %s", line)
            return True
        except Exception as exc:  # noqa: BLE001 - any serial failure is the same to us
            log.warning("serial write failed (%s); will reopen", exc)
            self._close_quietly()
            return False

    def _close_quietly(self) -> None:
        if self._port is not None:
            try:
                self._port.close()
            except Exception:  # noqa: BLE001, S110 - already failing; nothing useful to do
                pass
            self._port = None

    def _drain(self) -> None:
        """Log whatever the device said, so its diagnostics are not thrown away.

        The device narrates real things — `RESET BY WATCHDOG`, `transport usb TRUSTED`,
        `line too long` — and this is the only place a host can see them.
        """
        if self._port is None:
            return
        try:
            waiting = self._port.in_waiting
            if waiting:
                text = self._port.read(waiting).decode("ascii", "replace")
                for line in text.splitlines():
                    if line.strip():
                        log.debug("serial < %s", line.strip())
        except Exception as exc:  # noqa: BLE001
            log.warning("serial read failed (%s); will reopen", exc)
            self._close_quietly()

    def start(self) -> None:
        self._open()

    def heartbeat(self) -> None:
        """`host on`, and reopen the port if it went away.

        The device forgets us after 10 s of silence (D111), so this must be called well inside
        that. The daemon's 2 s loop does, giving five chances before trust is lost.
        """
        if self._port is None:
            try:
                self._open()
                self._warned = False
            except Exception as exc:  # noqa: BLE001
                if not self._warned:
                    # Once, not every two seconds: an unplugged device is a normal condition.
                    log.warning("serial transport unavailable (%s); retrying quietly", exc)
                    self._warned = True
                return

        self._drain()
        self._send("host on")

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        leaf = topic.rsplit("/", 1)[-1]

        if leaf == "state":
            state = payload.get("state")
            if isinstance(state, str) and state:
                self._send(f"state {state}")
            else:
                log.warning("no state in payload, nothing sent: %r", payload)
            return

        if leaf == "brightness":
            self._send(f"brightness {payload.get('value', '')}".strip())
            return

        # Anything else is MQTT-shaped and has no console equivalent; say so rather than guess.
        log.debug("no serial equivalent for %s, skipped", topic)

    def stop(self) -> None:
        """Say goodbye, which the device honours immediately rather than waiting out its timeout."""
        self._send("host off")
        self._close_quietly()
