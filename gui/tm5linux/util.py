"""Formatting helpers and per-user paths."""
from __future__ import annotations

import json
import os
import time


def fmt_bytes(n: float) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    v = float(n)
    u = 0
    while v >= 1024 and u < len(units) - 1:
        v /= 1024
        u += 1
    if u == 0:
        return f"{int(v)} B"
    return f"{v:.2f} {units[u]}" if v < 10 else f"{v:.1f} {units[u]}" if v < 100 else f"{v:.0f} {units[u]}"


def fmt_duration(seconds: float | None) -> str:
    if seconds is None or seconds < 0:
        return "–"
    s = int(seconds + 0.5)
    return f"{s // 3600}:{(s // 60) % 60:02d}:{s % 60:02d}"


def fmt_eta(seconds: float | None) -> str:
    """Rounded, friendly remaining time: '~ 38 min', '~ 1 h 05 min', '< 1 min'."""
    if seconds is None or seconds < 0:
        return "estimating…"
    if seconds < 60:
        return "< 1 min"
    minutes = int(seconds / 60 + 0.5)
    if minutes < 60:
        return f"~ {minutes} min"
    return f"~ {minutes // 60} h {minutes % 60:02d} min"


def fmt_speed(bps: float | None) -> str:
    if not bps or bps <= 0:
        return "–"
    return f"{bps / 1e9:.1f} GB/s"


def fmt_temp(t: float | None) -> str:
    return "–" if t is None else f"{t:.0f} °C"


def temps_text(cpu: float | None, ram: float | None) -> str:
    """'CPU 61 °C, RAM 47 °C' - only the parts that are known."""
    parts = []
    if cpu is not None:
        parts.append(f"CPU {fmt_temp(cpu)}")
    if ram is not None:
        parts.append(f"RAM {fmt_temp(ram)}")
    return ", ".join(parts)


def temp_tone(t: float | None, warn: float, hot: float) -> str | None:
    if t is None:
        return None
    return "bad" if t >= hot else "warn" if t >= warn else None


def clock() -> str:
    return time.strftime("%H:%M:%S")


def _xdg(var: str, fallback: str) -> str:
    v = os.environ.get(var, "")
    return v if v.startswith("/") else os.path.join(os.path.expanduser("~"), fallback)


def config_dir() -> str:
    return os.path.join(_xdg("XDG_CONFIG_HOME", ".config"), "tm5linux")


def user_configs_dir() -> str:
    return os.path.join(config_dir(), "configs")


def logs_dir() -> str:
    return os.path.join(_xdg("XDG_DATA_HOME", ".local/share"), "tm5linux", "logs")


class Settings:
    """Remembers the user's choices between runs (~/.config/tm5linux/settings.json)."""

    DEFAULTS = {
        "preset": "extreme",
        "memory_mode": "auto",
        "memory_mb": 4096,
        "threads": 0,
        "cycles": None,
        "time_scale": 100,
        "stop_on_error": False,
        "use_pkexec": False,
        "lock_warning_seen": False,
    }

    def __init__(self) -> None:
        self.path = os.path.join(config_dir(), "settings.json")
        self.data = dict(self.DEFAULTS)
        try:
            with open(self.path, encoding="utf-8") as f:
                stored = json.load(f)
            if isinstance(stored, dict):
                self.data.update({k: v for k, v in stored.items() if k in self.DEFAULTS})
        except (OSError, ValueError):
            pass

    def __getitem__(self, key):
        return self.data.get(key, self.DEFAULTS.get(key))

    def __setitem__(self, key, value) -> None:
        self.data[key] = value

    def save(self) -> None:
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=2)
            os.replace(tmp, self.path)
        except OSError:
            pass
