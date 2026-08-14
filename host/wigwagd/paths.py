"""Per-platform file locations.

Kept in one place so the daemon and the CLI cannot disagree about where the status
file lives, and so adding a platform means editing one function.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path


def status_path() -> Path:
    """Where the daemon writes its status snapshot for the CLI to read.

    Runtime state, not config, so it belongs in a runtime/cache location and is
    expected to vanish on reboot.
    """
    if override := os.environ.get("WIGWAG_STATUS_FILE"):
        return Path(override).expanduser()

    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
        return base / "wigwag" / "status.json"

    if runtime := os.environ.get("XDG_RUNTIME_DIR"):
        return Path(runtime) / "wigwag" / "status.json"

    # macOS has no XDG_RUNTIME_DIR. Use a uid-scoped temp dir so two users on one
    # machine cannot collide or read each other's session activity.
    uid = os.getuid() if hasattr(os, "getuid") else "shared"
    return Path(tempfile.gettempdir()) / f"wigwag-{uid}" / "status.json"
