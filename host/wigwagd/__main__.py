"""Entry point: ``uv run wigwagd`` (or ``python -m wigwagd``)."""

from __future__ import annotations

import argparse
import logging
import signal
import sys
import threading
import time

from . import __version__
from .config import Config, config_path
from .daemon import Daemon, write_status_file
from .listener import UdpListener
from .paths import status_path
from .publisher import MqttPublisher, NullPublisher, SerialPublisher

log = logging.getLogger("wigwagd")

_STATUS_INTERVAL = 2.0


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        prog="wigwagd",
        description="wigwag host daemon: aggregate AI session states and publish them over MQTT.",
    )
    p.add_argument("--config", type=str, default=None, help=f"config file (default: {config_path()})")
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="log what would be published instead of connecting to a broker; needs no paho-mqtt",
    )
    p.add_argument("-v", "--verbose", action="count", default=0, help="repeat for more detail")
    p.add_argument("--version", action="version", version=f"wigwagd {__version__}")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)

    level = logging.WARNING - min(args.verbose, 2) * 10  # WARNING → INFO → DEBUG
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    from pathlib import Path

    try:
        cfg = Config.load(Path(args.config) if args.config else None)
    except (OSError, ValueError) as exc:
        print(f"wigwagd: bad configuration: {exc}", file=sys.stderr)
        return 2

    log.info("config from %s", cfg.source)
    # Rule 4: say the uncomfortable thing rather than let a weak setup look fine. Broker warnings
    # are meaningless when the broker is not in use, so they are skipped rather than confusing.
    if not cfg.serial.enabled:
        for warning in cfg.broker.warnings():
            log.warning("%s", warning)

    # One transport, chosen explicitly (ADR-0018): a configured serial port means the device is
    # wired to this machine, and there is then no broker in the picture at all.
    if args.dry_run:
        publisher = NullPublisher()
    elif cfg.serial.enabled:
        log.info("wired transport: serial %s", cfg.serial.port)
        publisher = SerialPublisher(cfg)
    else:
        publisher = MqttPublisher(cfg)
    daemon = Daemon(cfg, publisher)

    try:
        publisher.start()
    except RuntimeError as exc:
        print(f"wigwagd: {exc}", file=sys.stderr)
        return 1

    listener = UdpListener(cfg.listen_host, cfg.listen_port, daemon.handle_datagram)
    try:
        listener.start()
    except RuntimeError as exc:
        print(f"wigwagd: {exc}", file=sys.stderr)
        publisher.stop()
        return 1

    stop = threading.Event()

    def _shutdown(signum, _frame) -> None:
        log.info("signal %s, shutting down", signal.Signals(signum).name)
        stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, _shutdown)

    spath = status_path()
    log.info("status file: %s", spath)
    daemon.publish_current()

    last_sweep = time.monotonic()
    try:
        while not stop.wait(_STATUS_INTERVAL):
            now = time.monotonic()
            if now - last_sweep >= 30.0:
                daemon.sweep()
                last_sweep = now
            # Tell the transport we are still here. A no-op over MQTT, where retention and the
            # Last Will already speak for us; `host on` over serial, which has neither and forgets
            # us after 10 s (D111). This loop's 2 s period gives five chances before that.
            try:
                publisher.heartbeat()
            except Exception as exc:  # noqa: BLE001 - a light must never take the daemon down
                log.warning("heartbeat failed: %s", exc)

            try:
                write_status_file(spath, daemon.snapshot())
            except OSError as exc:
                log.warning("cannot write status file %s: %s", spath, exc)
    finally:
        listener.stop()
        publisher.stop()
        log.info("stopped")

    return 0


if __name__ == "__main__":
    sys.exit(main())
