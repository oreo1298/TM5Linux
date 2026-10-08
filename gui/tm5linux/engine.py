"""Talks to the tm5linux-cli engine, which prints one JSON event per line."""
from __future__ import annotations

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
from typing import Callable

from gi.repository import GLib

ENGINE_NAME = "tm5linux-cli"


def find_engine(hint: str | None = None) -> str | None:
    """Locate the engine: $TM5LINUX_ENGINE, next to the launcher, the source tree, the install dir, $PATH."""
    candidates = [os.environ.get("TM5LINUX_ENGINE")]
    launcher_dir = os.path.dirname(os.path.realpath(sys.argv[0] or "."))
    candidates.append(os.path.join(launcher_dir, ENGINE_NAME))
    src_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
    candidates.append(os.path.join(src_root, "build", ENGINE_NAME))
    candidates.append(hint)
    candidates.append(shutil.which(ENGINE_NAME))
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.realpath(c)
    return None


def query(engine: str, *args: str, timeout: float = 20) -> list[dict]:
    """Run the engine once (e.g. --describe) and return its JSON events."""
    try:
        out = subprocess.run([engine, "--json", *args], capture_output=True, text=True, timeout=timeout,
                             stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired) as e:
        return [{"type": "finish", "result": "error", "message": str(e)}]
    events = []
    for line in out.stdout.splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            continue
    if out.returncode != 0 and not any(e.get("type") == "finish" for e in events):
        msg = out.stderr.strip() or f"engine exited with status {out.returncode}"
        events.append({"type": "finish", "result": "error", "message": msg})
    return events


def first(events: list[dict], kind: str) -> dict | None:
    return next((e for e in events if e.get("type") == kind), None)


class EngineRun:
    """One running test. Callbacks are always invoked on the GTK main thread."""

    def __init__(self, engine: str, args: list[str], on_event: Callable[[dict], None],
                 on_stderr: Callable[[str], None], on_exit: Callable[[int], None], use_pkexec: bool = False):
        self.engine = engine
        self.args = args
        self.on_event = on_event
        self.on_stderr = on_stderr
        self.on_exit = on_exit
        self.use_pkexec = use_pkexec
        self.proc: subprocess.Popen | None = None
        self.stopping = False
        self._kill_source = 0

    @property
    def command(self) -> list[str]:
        cmd = [self.engine, "--json", "--stdin-control", *self.args]
        if self.use_pkexec:
            cmd = ["pkexec", *cmd]
        return cmd

    def start(self) -> None:
        self.proc = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1, start_new_session=True)
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _read_stdout(self) -> None:
        assert self.proc and self.proc.stdout
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except ValueError:
                GLib.idle_add(self.on_stderr, line)
                continue
            GLib.idle_add(self.on_event, ev)
        code = self.proc.wait()
        GLib.idle_add(self._exited, code)

    def _read_stderr(self) -> None:
        assert self.proc and self.proc.stderr
        for line in self.proc.stderr:
            line = line.rstrip()
            if line:
                GLib.idle_add(self.on_stderr, line)

    def _exited(self, code: int) -> bool:
        if self._kill_source:
            GLib.source_remove(self._kill_source)
            self._kill_source = 0
        self.on_exit(code)
        return False

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def stop(self) -> None:
        """Ask the engine to finish the current block and exit; force it after a timeout."""
        if not self.running or self.stopping:
            return
        self.stopping = True
        try:
            assert self.proc and self.proc.stdin
            self.proc.stdin.write("stop\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            pass
        self._kill_source = GLib.timeout_add_seconds(20, self._force)

    def _force(self) -> bool:
        self._kill_source = 0
        if self.running and self.proc:
            try:
                if self.use_pkexec:
                    self.proc.stdin.close()      # cannot signal a root process; EOF makes it stop
                else:
                    os.killpg(self.proc.pid, signal.SIGKILL)
            except (OSError, ValueError):
                pass
        return False

    def kill_now(self) -> None:
        if not self.running or not self.proc:
            return
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass
        if not self.use_pkexec:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass
