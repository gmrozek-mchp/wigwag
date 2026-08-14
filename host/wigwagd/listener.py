"""UDP datagram listener.

UDP on loopback rather than a Unix domain socket, for two reasons (ADR-0010):

* **Portability.** AF_UNIX *datagram* sockets do not exist on Windows, and bash's
  `/dev/udp` — which is how the hook client sends without needing `nc` — only speaks
  UDP/TCP.
* **Fire-and-forget.** `sendto` on a connectionless socket cannot block and cannot
  fail because nobody is listening, so a hook can never hang or error out because the
  daemon is down. That is Rule 3, enforced by the transport rather than by care.
"""

from __future__ import annotations

import logging
import socket
import threading
from collections.abc import Callable

from .protocol import MAX_DATAGRAM

log = logging.getLogger(__name__)


class UdpListener:
    """Receives datagrams on a background thread and hands them to a callback.

    Binds to loopback by default: this is a local IPC channel, and there is no reason
    for anything off-machine to be able to drive the light.
    """

    def __init__(
        self,
        host: str,
        port: int,
        on_datagram: Callable[[bytes], None],
    ) -> None:
        self._host = host
        self._port = port
        self._on_datagram = on_datagram
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()

    @property
    def port(self) -> int:
        """The bound port. Differs from the requested one when port 0 was asked for,
        which is how tests get a free port without racing."""
        if self._sock is None:
            return self._port
        return self._sock.getsockname()[1]

    def start(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((self._host, self._port))
        except OSError as exc:
            sock.close()
            raise RuntimeError(
                f"cannot bind {self._host}:{self._port} — is another wigwagd running? ({exc})"
            ) from exc
        # Timeout so the thread notices _stop promptly instead of blocking forever.
        sock.settimeout(0.5)
        self._sock = sock

        self._thread = threading.Thread(target=self._run, name="wigwag-udp", daemon=True)
        self._thread.start()
        log.info("listening for hook datagrams on %s:%d", self._host, self.port)

    def _run(self) -> None:
        assert self._sock is not None
        while not self._stop.is_set():
            try:
                data, _addr = self._sock.recvfrom(MAX_DATAGRAM)
            except socket.timeout:
                continue
            except OSError:
                if self._stop.is_set():
                    return
                log.exception("recvfrom failed")
                continue
            try:
                self._on_datagram(data)
            except Exception:
                # A bad datagram must never take down the listener. The sender is a
                # hook that cannot see our errors and must not be affected by them.
                log.exception("handler raised on datagram %r", data[:64])

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            self._sock.close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self._sock = None
        self._thread = None
