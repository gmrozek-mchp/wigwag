"""MQTT publishing, behind an interface so the daemon is testable without a broker.

`paho-mqtt` is imported lazily inside `MqttPublisher` so the pure logic, the protocol
parser and the whole test suite run with no third-party dependency installed at all.
"""

from __future__ import annotations

import json
import logging
from typing import Protocol

log = logging.getLogger(__name__)


class Publisher(Protocol):
    """What the daemon needs from a transport."""

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        """Publish the displayed state, **retained**.

        Retention is the point (ADR-0003): the broker replays this to the device the
        instant it subscribes, so the light is correct immediately after a reboot or
        a dropped link, with no host involvement.
        """
        ...

    def start(self) -> None: ...

    def stop(self) -> None: ...


class NullPublisher:
    """Records instead of publishing. Used by tests and by ``--dry-run``."""

    def __init__(self) -> None:
        self.published: list[tuple[str, dict[str, object]]] = []
        self.started = False

    def publish_state(self, topic: str, payload: dict[str, object]) -> None:
        self.published.append((topic, payload))
        log.info("would publish %s %s", topic, json.dumps(payload, sort_keys=True))

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
