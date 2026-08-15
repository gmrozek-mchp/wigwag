"""Configuration: TOML file plus environment overrides.

The broker may be local or remote (ADR-0011), so every knob is settable and the
defaults are the local case. Requires Python 3.11+ for ``tomllib``.
"""

from __future__ import annotations

import ipaddress
import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_LISTEN_HOST = "127.0.0.1"
DEFAULT_LISTEN_PORT = 9410
DEFAULT_MQTT_PORT = 1883
DEFAULT_MQTT_TLS_PORT = 8883


def config_path() -> Path:
    """Where the config lives, per-platform.

    ``WIGWAG_CONFIG`` wins if set. Otherwise Windows uses ``%APPDATA%``, and
    everything else follows the XDG default of ``~/.config``.
    """
    if override := os.environ.get("WIGWAG_CONFIG"):
        return Path(override).expanduser()
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "wigwag" / "config.toml"


def _is_loopback(host: str) -> bool:
    """Whether a broker host is on this machine.

    Used only to decide whether TLS is required by default. Unresolvable or
    non-numeric hosts are treated as remote, which is the safe direction: it turns
    TLS on rather than off.
    """
    if host in ("localhost", ""):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


@dataclass(slots=True)
class BrokerConfig:
    """How to reach the MQTT broker — local or anywhere else."""

    host: str = "localhost"
    port: int | None = None  # None → chosen from `tls`
    tls: bool | None = None  # None → inferred from `host`
    ca_cert: str | None = None
    insecure_skip_verify: bool = False
    username: str | None = None
    password: str | None = None
    client_id: str = "wigwagd"
    keepalive: int = 30

    def resolve(self) -> None:
        """Fill in the values that depend on other values.

        **TLS defaults on for any non-loopback broker.** Pointing wigwag at a remote
        broker should not quietly ship credentials and session activity in the clear;
        you can still turn it off explicitly, and `warnings()` will complain.
        """
        if self.tls is None:
            self.tls = not _is_loopback(self.host)
        if self.port is None:
            self.port = DEFAULT_MQTT_TLS_PORT if self.tls else DEFAULT_MQTT_PORT

    def warnings(self) -> list[str]:
        """Configuration that is legal but worth saying out loud.

        Consistent with Rule 4: surface the uncomfortable truth rather than let a
        silently-insecure setup look fine.
        """
        out: list[str] = []
        remote = not _is_loopback(self.host)
        if remote and not self.tls:
            out.append(
                f"broker {self.host}:{self.port} is remote but TLS is disabled — "
                "credentials and session activity will cross the network in the clear"
            )
        if self.tls and self.insecure_skip_verify:
            out.append(
                "insecure_skip_verify is on — TLS is encrypting but not authenticating "
                "the broker, so this does not protect against interception"
            )
        if remote and not self.username:
            out.append(f"broker {self.host} is remote but no username is configured")
        return out


@dataclass(slots=True)
class SerialConfig:
    """The wired transport (ADR-0018, ADR-0020).

    ``port`` empty means "use MQTT", which is the default. Setting it selects the serial
    transport outright — deliberately explicit, because guessing which of two transports the
    user meant is exactly the kind of cleverness that produces a light that silently does
    nothing.

    ``port = "auto"`` asks for discovery by the MCP2221A's factory USB identity
    (VID 0x04D8, PID 0x00DD, from the datasheet's USBVID/USBPID registers). Discovery fails
    loudly when it finds none or several, rather than picking one.
    """

    port: str = ""
    baud: int = 115200

    @property
    def enabled(self) -> bool:
        return bool(self.port.strip())


@dataclass(slots=True)
class Config:
    broker: BrokerConfig = field(default_factory=BrokerConfig)
    serial: SerialConfig = field(default_factory=SerialConfig)
    topic_prefix: str = "wigwag"
    listen_host: str = DEFAULT_LISTEN_HOST
    listen_port: int = DEFAULT_LISTEN_PORT
    session_ttl: float = 900.0
    source: str = "defaults"

    def topic(self, leaf: str) -> str:
        return f"{self.topic_prefix}/{leaf}"

    @classmethod
    def load(cls, path: Path | None = None) -> Config:
        """Load defaults, then the TOML file if present, then environment overrides.

        A missing config file is normal, not an error: the defaults are a working
        local setup.
        """
        cfg = cls()
        path = path or config_path()

        if path.is_file():
            with path.open("rb") as fh:
                data = tomllib.load(fh)
            cfg._apply_toml(data)
            cfg.source = str(path)

        cfg._apply_env()
        cfg.broker.resolve()
        cfg._validate()
        return cfg

    def _apply_toml(self, data: dict[str, object]) -> None:
        broker = data.get("broker", {})
        if isinstance(broker, dict):
            for key in (
                "host", "port", "tls", "ca_cert", "insecure_skip_verify",
                "username", "password", "client_id", "keepalive",
            ):
                if key in broker:
                    setattr(self.broker, key, broker[key])

        serial_ = data.get("serial", {})
        if isinstance(serial_, dict):
            if "port" in serial_:
                self.serial.port = str(serial_["port"])
            if "baud" in serial_:
                self.serial.baud = int(serial_["baud"])  # type: ignore[arg-type]

        topics = data.get("topics", {})
        if isinstance(topics, dict) and "prefix" in topics:
            self.topic_prefix = str(topics["prefix"])

        daemon = data.get("daemon", {})
        if isinstance(daemon, dict):
            if "listen_host" in daemon:
                self.listen_host = str(daemon["listen_host"])
            if "listen_port" in daemon:
                self.listen_port = int(daemon["listen_port"])  # type: ignore[arg-type]
            if "session_ttl" in daemon:
                self.session_ttl = float(daemon["session_ttl"])  # type: ignore[arg-type]

    def _apply_env(self) -> None:
        """Environment overrides. Handy for CI and for keeping a password out of a file."""
        env = os.environ

        def _bool(raw: str) -> bool:
            return raw.strip().lower() in ("1", "true", "yes", "on")

        if v := env.get("WIGWAG_BROKER_HOST"):
            self.broker.host = v
        if v := env.get("WIGWAG_BROKER_PORT"):
            self.broker.port = int(v)
        if v := env.get("WIGWAG_BROKER_TLS"):
            self.broker.tls = _bool(v)
        if v := env.get("WIGWAG_BROKER_CA_CERT"):
            self.broker.ca_cert = v
        if v := env.get("WIGWAG_MQTT_USERNAME"):
            self.broker.username = v
        if v := env.get("WIGWAG_MQTT_PASSWORD"):
            self.broker.password = v
        if v := env.get("WIGWAG_SERIAL_PORT"):
            self.serial.port = v
        if v := env.get("WIGWAG_SERIAL_BAUD"):
            self.serial.baud = int(v)
        if v := env.get("WIGWAG_TOPIC_PREFIX"):
            self.topic_prefix = v
        if v := env.get("WIGWAG_LISTEN_PORT"):
            self.listen_port = int(v)
        if v := env.get("WIGWAG_SESSION_TTL"):
            self.session_ttl = float(v)

    def _validate(self) -> None:
        if not self.topic_prefix.strip():
            raise ValueError("topic_prefix must not be empty")
        if any(c in self.topic_prefix for c in "+#"):
            raise ValueError(f"topic_prefix must not contain MQTT wildcards: {self.topic_prefix!r}")
        if not (0 < self.listen_port < 65536):
            raise ValueError(f"listen_port out of range: {self.listen_port}")
        if self.broker.port is not None and not (0 < self.broker.port < 65536):
            raise ValueError(f"broker.port out of range: {self.broker.port}")
        if self.session_ttl <= 0:
            raise ValueError(f"session_ttl must be positive, got {self.session_ttl}")
        if self.serial.enabled and self.serial.baud <= 0:
            raise ValueError(f"serial.baud must be positive, got {self.serial.baud}")
