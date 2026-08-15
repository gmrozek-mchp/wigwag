"""``wigwag`` — the generic push API and status view.

This is what lets anything drive the light: CI, a PR bot, a long build, cron. It
speaks the same UDP protocol as the hook client, so producers are interchangeable
and the daemon cannot tell them apart (CONTEXT.md: "producer").

Not in the hook path, so a Python start-up cost is fine here. The hook client is
deliberately something else entirely (ADR-0010).
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path

from . import __version__
from .config import Config
from .paths import status_path
from .state import State


def _send(cfg: Config, line: str) -> None:
    """Fire one datagram at the daemon. Cannot block; cannot fail if it is down."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(line.encode("utf-8"), (cfg.listen_host, cfg.listen_port))


def _read_status() -> dict | None:
    path = status_path()
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, json.JSONDecodeError):
        return None


def _cmd_set(cfg: Config, args: argparse.Namespace) -> int:
    try:
        state = State.parse(args.state)
    except ValueError as exc:
        print(f"wigwag: {exc}", file=sys.stderr)
        return 2
    _send(cfg, f"SET {state.name} {args.id} {args.reason}")
    if not args.quiet:
        print(f"{state.name} (producer {args.id!r}, reason {args.reason!r})")
    return 0


def _cmd_clear(cfg: Config, args: argparse.Namespace) -> int:
    _send(cfg, f"DROP {args.id}")
    if not args.quiet:
        print(f"cleared producer {args.id!r}")
    return 0


def _cmd_status(_cfg: Config, args: argparse.Namespace) -> int:
    snap = _read_status()
    if snap is None:
        print("wigwag: no status available — is wigwagd running?", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(snap, indent=2, sort_keys=True))
        return 0

    agg = snap.get("aggregate", {})
    print(f"{agg.get('state', '?'):<6} {agg.get('reason', '')}")
    print(f"transport  {snap.get('transport', '?')}")
    print(f"config  {snap.get('config_source', '?')}")
    sessions = snap.get("sessions", {})
    if not sessions:
        print("no live sessions")
        return 0
    print(f"{len(sessions)} live session(s):")
    for sid, s in sorted(sessions.items(), key=lambda kv: -kv[1].get("age_s", 0)):
        print(f"  {s.get('state', '?'):<6} {sid:<40} {s.get('age_s', '?')}s  {s.get('reason', '')}")
    return 0


def _cmd_watch(_cfg: Config, args: argparse.Namespace) -> int:
    """Poll the status file and print the aggregate whenever it changes."""
    last: str | None = None
    try:
        while True:
            snap = _read_status()
            if snap is None:
                current = "(daemon not running)"
            else:
                agg = snap.get("aggregate", {})
                current = f"{agg.get('state', '?')} {agg.get('reason', '')} [{agg.get('sessions', 0)}]"
            if current != last:
                print(f"{time.strftime('%H:%M:%S')}  {current}", flush=True)
                last = current
            time.sleep(args.interval)
    except KeyboardInterrupt:
        return 0


def _cmd_config(cfg: Config, _args: argparse.Namespace) -> int:
    b = cfg.broker
    print(f"source        {cfg.source}")

    # Only describe the transport in use. Printing broker settings for a wired device invites
    # debugging the wrong half of the system, and printing its warnings is worse than useless.
    if cfg.serial.enabled:
        print(f"transport     serial {cfg.serial.port} at {cfg.serial.baud}")
    else:
        print(f"transport     mqtt {b.host}:{b.port}")
        print(f"tls           {b.tls}")
        print(f"auth          {'username ' + b.username if b.username else 'none'}")
        print(f"topic prefix  {cfg.topic_prefix}")

    print(f"listen        {cfg.listen_host}:{cfg.listen_port}")
    print(f"session ttl   {cfg.session_ttl:g}s")
    print(f"status file   {status_path()}")

    if not cfg.serial.enabled:
        for w in b.warnings():
            print(f"warning: {w}", file=sys.stderr)
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="wigwag",
        description="Drive the wigwag status light, or inspect what it is showing.",
        epilog="Anything can be a producer: `wigwag set ERROR --id ci --reason build-failed`",
    )
    p.add_argument("--config", type=str, default=None, help="config file")
    p.add_argument("--version", action="version", version=f"wigwag {__version__}")
    sub = p.add_subparsers(dest="command", required=True)

    s = sub.add_parser("set", help="set a producer's state")
    s.add_argument("state", help="IDLE, BUSY, WAIT or ERROR (case-insensitive)")
    s.add_argument("--id", default="cli", help="producer id (default: cli)")
    s.add_argument("--reason", default="manual", help="diagnostic label")
    s.add_argument("-q", "--quiet", action="store_true")
    s.set_defaults(func=_cmd_set)

    c = sub.add_parser("clear", help="remove a producer so it stops voting")
    c.add_argument("--id", default="cli")
    c.add_argument("-q", "--quiet", action="store_true")
    c.set_defaults(func=_cmd_clear)

    st = sub.add_parser("status", help="show the displayed state and live sessions")
    st.add_argument("--json", action="store_true")
    st.set_defaults(func=_cmd_status)

    w = sub.add_parser("watch", help="print the aggregate whenever it changes")
    w.add_argument("--interval", type=float, default=0.5)
    w.set_defaults(func=_cmd_watch)

    cf = sub.add_parser("config", help="show effective configuration")
    cf.set_defaults(func=_cmd_config)

    args = p.parse_args(argv)
    try:
        cfg = Config.load(Path(args.config) if args.config else None)
    except (OSError, ValueError) as exc:
        print(f"wigwag: bad configuration: {exc}", file=sys.stderr)
        return 2
    return int(args.func(cfg, args))


if __name__ == "__main__":
    sys.exit(main())
