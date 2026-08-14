#!/usr/bin/env python3
"""Fake RNWF02 module: speaks the AT protocol on a serial line and bridges MQTT to a real broker.

This is the other half of ADR-0015. The firmware's AT client talks to this instead of a real
module, either over a PTY (host-only testing, no hardware at all) or over a USB-UART adapter to a
PL10 Curiosity Nano running the real firmware.

    # host-only, no hardware: prints the PTY path for the POSIX adapter to open
    python3 fake_rnwf02.py --pty --broker localhost

    # against real hardware over a USB-UART adapter
    python3 fake_rnwf02.py --port /dev/cu.usbserial-XXXX --baud 115200 --broker localhost

    # failure injection, which is the thing a real module will not do on demand
    python3 fake_rnwf02.py --pty --no-connack          # accept MQTTCONN, never acknowledge
    python3 fake_rnwf02.py --pty --connack-reason 130  # protocol error
    python3 fake_rnwf02.py --pty --fail AT+MQTTSUB     # ERROR:<code> to one command
    python3 fake_rnwf02.py --pty --drop-link-after 20  # +WSTALD once connected

Framing is deliberately faithful to the AT Command Specification (Network Controller 3.1.0,
Revision 58a15dc2), because the framing is where the client is most likely to be wrong:

  * responses are  <RESPONSE><CR><LF>
  * AECs are       <CR>+NAME:INFO<CR><LF>   -- note the *leading* CR
  * AECs are never emitted while a command is being processed, but may queue up behind one

WHAT THIS CANNOT TELL YOU: it encodes *our model* of the module, so it can prove the client's
framing, sequencing, timeout and backoff logic, but it cannot settle anything the specification
leaves unsaid. In particular the module's escaping of a double quote inside a quoted AEC field is
unspecified, and this fake simply passes payloads through. Only the real module answers that.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import argparse
import os
import queue
import re
import select
import sys
import time

# Status code used for injected failures. 1 is a generic failure in the spec's table; the client
# only logs the number, so the exact value matters less than the shape "ERROR:<n>".
INJECTED_ERROR_CODE = 1


class Link:
    """A byte-oriented serial link: either a PTY pair or a real serial port."""

    def __init__(self, fd: int, closer=None) -> None:
        self.fd = fd
        self._closer = closer

    def write(self, data: bytes) -> None:
        os.write(self.fd, data)

    def read(self, timeout: float) -> bytes:
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 512)
        except OSError:
            return b""

    def close(self) -> None:
        if self._closer:
            self._closer()
        else:
            os.close(self.fd)

    @staticmethod
    def open_pty() -> "Link":
        import pty

        primary, secondary = pty.openpty()
        path = os.ttyname(secondary)
        print(f"fake_rnwf02: PTY ready at {path}", flush=True)
        return Link(primary)

    @staticmethod
    def open_serial(port: str, baud: int) -> "Link":
        try:
            import serial  # type: ignore
        except ImportError:
            sys.exit("fake_rnwf02: --port needs pyserial (uv pip install pyserial)")

        ser = serial.Serial(port, baud, timeout=0)
        print(f"fake_rnwf02: serial ready on {port} at {baud}", flush=True)
        return Link(ser.fileno(), closer=ser.close)


class Broker:
    """MQTT bridge to a real broker. Optional: --no-broker loopbacks instead."""

    def __init__(self, host: str, port: int, aecs: "queue.Queue[bytes]") -> None:
        self.host = host
        self.port = port
        self.aecs = aecs
        self.client = None
        self.subscriptions: list[str] = []

    def connect(self, cfg: dict, lwt: tuple | None) -> bool:
        try:
            import paho.mqtt.client as mqtt  # lazy, matching the host project's convention (D31)
        except ImportError:
            sys.exit("fake_rnwf02: needs paho-mqtt, or pass --no-broker")

        # Tear down any previous session first. A real module has exactly one MQTT connection, and
        # leaking one per AT+MQTTCONN gave two clients sharing a client_id, which a broker resolves
        # by evicting whichever connected first - so they ping-pong and delivery becomes flaky.
        # Observed as "the device connects fine but later publishes never arrive".
        if self.client is not None:
            self.close()
            self.client = None
            self.subscriptions.clear()

        client_id = cfg.get(3, "fake-rnwf02")

        # CallbackAPIVersion is required by paho 2.x; guard so paho 1.x also works. Same shape as
        # host/wigwagd/publisher.py, deliberately — one convention for the whole project.
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
        else:  # pragma: no cover - paho 1.x fallback
            self.client = mqtt.Client(client_id=client_id)

        if cfg.get(4):
            self.client.username_pw_set(cfg.get(4), cfg.get(5) or None)

        if lwt:
            qos, retain, topic, payload = lwt
            # The device registered a Last Will via AT+MQTTLWT; a real module would pass it into
            # its own CONNECT, so the fake must too or the LWT path is never actually exercised.
            self.client.will_set(topic, payload, qos=int(qos), retain=bool(int(retain)))

        self.client.on_message = self._on_message

        host = cfg.get(1, self.host)
        port = int(cfg.get(2, self.port))
        keepalive = int(cfg.get(6, 60)) or 60

        try:
            self.client.connect(host, port, keepalive)
        except OSError as err:
            print(f"fake_rnwf02: broker connect failed: {err}", flush=True)
            return False

        self.client.loop_start()
        return True

    def _on_message(self, _client, _userdata, msg) -> None:
        payload = msg.payload.decode("utf-8", "replace")
        retain = 1 if msg.retain else 0
        # +MQTTSUBRX:<DUP>,<QOS>,<RETAIN>,<TOPIC_NAME>,<TOPIC_PAYLOAD>
        self.aecs.put(
            aec(f'+MQTTSUBRX:0,{msg.qos},{retain},"{msg.topic}","{payload}"')
        )

    def subscribe(self, topic: str, qos: int) -> None:
        self.subscriptions.append(topic)
        if self.client:
            self.client.subscribe(topic, qos)

    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> None:
        if self.client:
            self.client.publish(topic, payload, qos=qos, retain=retain)

    def close(self) -> None:
        if self.client:
            self.client.loop_stop()
            self.client.disconnect()


def resp(text: str) -> bytes:
    """A final result code or command response: <RESPONSE><CR><LF>."""
    return text.encode() + b"\r\n"


def aec(text: str) -> bytes:
    """An asynchronous event code: <CR>+NAME:INFO<CR><LF>, leading CR included."""
    return b"\r" + text.encode() + b"\r\n"


def split_args(args: str) -> list[str]:
    """Split an AT argument list on commas that are not inside double quotes."""
    out, cur, in_q = [], [], False
    for ch in args:
        if ch == '"':
            in_q = not in_q
            cur.append(ch)
        elif ch == "," and not in_q:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def unquote(s: str) -> str:
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


class FakeModule:
    def __init__(self, args, link: Link) -> None:
        self.args = args
        self.link = link
        self.aecs: "queue.Queue[bytes]" = queue.Queue()
        self.broker = None if args.no_broker else Broker(args.broker, args.port_num, self.aecs)

        self.verbosity = 2
        self.wstac: dict[int, str] = {}
        self.mqttc: dict[int, str] = {}
        self.lwt: tuple | None = None
        self.wifi_up = False
        self.mqtt_up = False
        self.assoc_id = 1
        self.link_drop_at: float | None = None
        self.failed_once: set[str] = set()

    # ---------------------------------------------------------------- output

    def send(self, data: bytes) -> None:
        if self.args.verbose:
            print(f"  -> {data!r}", flush=True)
        self.link.write(data)

    def ok(self) -> None:
        self.send(resp("OK" if self.verbosity >= 2 else "0"))

    def error(self, code: int = INJECTED_ERROR_CODE) -> None:
        if self.verbosity >= 3:
            self.send(resp(f"ERROR:{code}"))
        elif self.verbosity == 2:
            self.send(resp("ERROR"))
        else:
            self.send(resp(f"1:{code}"))

    def flush_aecs(self) -> None:
        """Emit queued AECs. Called only between commands, as the specification requires."""
        while True:
            try:
                self.send(self.aecs.get_nowait())
            except queue.Empty:
                return

    # --------------------------------------------------------------- commands

    def handle(self, line: str) -> None:
        if self.args.verbose:
            print(f"  <- {line!r}", flush=True)

        if self.args.fail and line.startswith(self.args.fail) and self.args.fail not in self.failed_once:
            self.failed_once.add(self.args.fail)
            print(f"fake_rnwf02: injecting ERROR for {line!r}", flush=True)
            self.error()
            return

        if self.args.slow:
            time.sleep(self.args.slow)

        # ATV<n> — verbosity. Must be honoured, because it changes every later error's shape.
        m = re.fullmatch(r"ATV(\d)", line)
        if m:
            self.verbosity = int(m.group(1))
            self.ok()
            return

        if line == "AT":
            self.ok()
            return

        if line == "AT+RST":
            self.ok()
            self.reset_state()
            self.aecs.put(aec("+BOOT:RNWF02 fake, Network Controller 3.1.0"))
            return

        if line == "AT+GMR":
            self.send(resp("+GMR:3.1.0"))
            self.ok()
            return

        if line.startswith("AT+WSTAC="):
            args = split_args(line[len("AT+WSTAC="):])
            if len(args) >= 2:
                self.wstac[int(args[0])] = unquote(args[1])
            self.ok()
            return

        if line.startswith("AT+WSTA="):
            state = unquote(line[len("AT+WSTA="):])
            self.ok()
            if state == "1":
                self.bring_wifi_up()
            else:
                self.wifi_up = False
            return

        if line.startswith("AT+MQTTC="):
            args = split_args(line[len("AT+MQTTC="):])
            if len(args) >= 2:
                self.mqttc[int(args[0])] = unquote(args[1])
            self.ok()
            return

        if line.startswith("AT+MQTTLWT="):
            args = split_args(line[len("AT+MQTTLWT="):])
            if len(args) >= 4:
                self.lwt = (args[0], args[1], unquote(args[2]), unquote(args[3]))
                print(f"fake_rnwf02: LWT registered {self.lwt}", flush=True)
            self.ok()
            return

        if line.startswith("AT+MQTTCONN"):
            self.ok()
            self.connect_mqtt()
            return

        if line.startswith("AT+MQTTSUB="):
            args = split_args(line[len("AT+MQTTSUB="):])
            topic = unquote(args[0]) if args else ""
            qos = int(args[1]) if len(args) > 1 else 0
            self.ok()
            if self.broker:
                self.broker.subscribe(topic, qos)
            print(f"fake_rnwf02: subscribed {topic} qos {qos}", flush=True)
            return

        if line.startswith("AT+MQTTPUB="):
            args = split_args(line[len("AT+MQTTPUB="):])
            if len(args) >= 5:
                _dup, qos, retain, topic = args[0], int(args[1]), args[2], unquote(args[3])
                # The payload is the tail, so a payload containing commas survives.
                payload = unquote(",".join(args[4:]))
                if self.broker:
                    self.broker.publish(topic, payload, qos, retain == "1")
                print(f"fake_rnwf02: published {topic} = {payload!r}", flush=True)
            self.ok()
            return

        if line.startswith("AT+MQTTDISCONN"):
            self.mqtt_up = False
            self.ok()
            return

        # Unknown command. A real module answers with an error, not silence.
        self.error()

    # ---------------------------------------------------------------- states

    def reset_state(self) -> None:
        self.wifi_up = False
        self.mqtt_up = False
        self.wstac.clear()
        self.mqttc.clear()
        self.lwt = None
        self.link_drop_at = None

    def bring_wifi_up(self) -> None:
        ssid = self.wstac.get(1, "?")
        self.wifi_up = True
        self.aecs.put(aec(f'+WSTALU:{self.assoc_id},"AA:BB:CC:DD:EE:FF",6'))
        self.aecs.put(aec(f'+WSTAAIP:{self.assoc_id},"192.168.1.42"'))
        print(f"fake_rnwf02: wifi up on {ssid!r}", flush=True)

        if self.args.drop_link_after:
            self.link_drop_at = time.monotonic() + self.args.drop_link_after

    def connect_mqtt(self) -> None:
        if self.args.no_connack:
            print("fake_rnwf02: accepting MQTTCONN and withholding CONNACK", flush=True)
            return

        if self.args.connack_reason:
            self.aecs.put(aec(f"+MQTTCONNACK:0,{self.args.connack_reason}"))
            return

        if self.broker:
            if not self.broker.connect(self.mqttc, self.lwt):
                self.aecs.put(aec("+MQTTCONNACK:0,128"))
                return

        self.mqtt_up = True
        self.aecs.put(aec("+MQTTCONNACK:0,0"))
        host = self.mqttc.get(1, "-")
        print(f"fake_rnwf02: MQTT connected to {host}", flush=True)

    def maybe_drop_link(self) -> None:
        if self.link_drop_at and time.monotonic() >= self.link_drop_at:
            self.link_drop_at = None
            self.wifi_up = False
            self.mqtt_up = False
            print("fake_rnwf02: dropping the Wi-Fi link (injected)", flush=True)
            self.aecs.put(aec(f"+WSTALD:{self.assoc_id}"))

    # ------------------------------------------------------------- main loop

    def run(self) -> None:
        buf = bytearray()
        try:
            while True:
                data = self.link.read(timeout=0.1)
                for b in data:
                    if b in (0x0D, 0x0A):
                        if buf:
                            self.handle(buf.decode("utf-8", "replace"))
                            buf.clear()
                    else:
                        buf.append(b)

                self.maybe_drop_link()
                self.flush_aecs()
        except KeyboardInterrupt:
            pass
        finally:
            if self.broker:
                self.broker.close()
            self.link.close()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--pty", action="store_true", help="serve on a new PTY and print its path")
    src.add_argument("--port", help="serial device, e.g. /dev/cu.usbserial-XXXX")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--broker", default="localhost", help="fallback broker host")
    p.add_argument("--port-num", type=int, default=1883, help="fallback broker port")
    p.add_argument("--no-broker", action="store_true", help="do not touch a real broker")
    p.add_argument("-v", "--verbose", action="store_true", help="log every line both ways")

    inj = p.add_argument_group("failure injection")
    inj.add_argument("--fail", metavar="PREFIX", help="answer ERROR once to the first match")
    inj.add_argument("--no-connack", action="store_true", help="accept MQTTCONN, never ack")
    inj.add_argument("--connack-reason", type=int, help="fail CONNACK with this reason code")
    inj.add_argument("--drop-link-after", type=float, metavar="SEC", help="emit +WSTALD")
    inj.add_argument("--slow", type=float, metavar="SEC", help="delay before every response")

    args = p.parse_args()
    link = Link.open_pty() if args.pty else Link.open_serial(args.port, args.baud)
    FakeModule(args, link).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
