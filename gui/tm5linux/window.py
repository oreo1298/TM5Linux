"""The main window: settings on the left, live status of the test on the right."""
from __future__ import annotations

import os
import shutil
import threading
import time

from gi.repository import Gio, GLib, Gtk

from . import APP_NAME, VERSION
from .engine import EngineRun, find_engine, first, query
from .util import (Settings, fmt_bytes, fmt_duration, fmt_eta, fmt_speed, fmt_temp, logs_dir, temp_tone, temps_text,
                   user_configs_dir)
from .widgets import (Banner, ErrorList, LabeledBar, LogView, SequenceView, StatCard, ThreadsView, hbox, label,
                      set_tone, vbox)

PROFILE_NOTES = {
    "extreme": "The most popular TM5 profile for RAM overclocking (by anta777). Thorough and demanding - "
               "let it finish all cycles before trusting new settings.",
    "absolut": "A heavier anta777 profile with many more repetitions. Takes longer than Extreme; "
               "good as a final check.",
    "universal2": "LMHz's quicker, broad profile. Handy for checking changes between longer runs.",
    "pcbdestroyer": "32 cycles of heavy copy tests by Mechan0. Very long - for final stability checks.",
}

CPU_TEMP_TIP = ("CPU temperature ({sensor}). Memory tests mostly wait on the RAM, so the CPU usually stays well "
                "below its limit. Yellow from 85 °C, red from 95 °C.")
CPU_NO_SENSOR_TIP = ("No CPU temperature sensor was found. Loading the driver usually fixes this: "
                     "'sudo modprobe k10temp' (AMD) or 'sudo modprobe coretemp' (Intel).")
RAM_TEMP_TIP = ("Temperature of your memory modules ({sensor}); the hottest one is shown.\n{modules}\n\nMemory errors "
                "get more likely as modules heat up - for overclocked RAM many people aim to stay below about 50 °C. "
                "Yellow from 50 °C, red from 60 °C.")

DURATIONS = [(25, "Quick - 25 %"), (50, "Short - 50 %"), (100, "Normal - 100 % (as the profile says)"),
             (200, "Long - 200 %"), (400, "Very long - 400 %")]


def ask(parent: Gtk.Window, title: str, text: str, buttons: list[tuple[str, str, str]], callback,
        command: str | None = None) -> Gtk.Window:
    """A small modal dialog. buttons: (response id, label, css class); callback(response id)."""
    win = Gtk.Window(transient_for=parent, modal=True, title=title, resizable=False)
    win.add_css_class("tm5-dialog")
    win.set_default_size(500, -1)
    box = vbox(14)
    for m in ("top", "bottom", "start", "end"):
        getattr(box, f"set_margin_{m}")(24)
    box.append(label(title, "dialog-title", wrap=True))
    msg = label(text, "caption", wrap=True)
    msg.set_max_width_chars(64)
    box.append(msg)
    if command:
        cmd = label(command, "command", wrap=True, selectable=True)
        box.append(cmd)
    row = hbox(8)
    row.set_halign(Gtk.Align.END)
    row.set_margin_top(6)
    done = {"fired": False}

    def fire(rid: str) -> None:
        if not done["fired"]:
            done["fired"] = True
            win.destroy()
            if callback:
                callback(rid)

    if command:
        copy = Gtk.Button(label="Copy command")
        copy.connect("clicked", lambda *_: (parent.get_clipboard().set(command), copy.set_label("Copied ✔")))
        row.append(copy)
    for rid, text_, css in buttons:
        b = Gtk.Button(label=text_)
        if css:
            b.add_css_class(css)
        b.connect("clicked", lambda _b, r=rid: fire(r))
        row.append(b)
    box.append(row)
    win.set_child(box)
    win.connect("close-request", lambda *_: (fire("cancel"), False)[1])
    win.present()
    return win


class MainWindow(Gtk.ApplicationWindow):
    def __init__(self, app: Gtk.Application, engine_hint: str | None = None):
        super().__init__(application=app, title=APP_NAME)
        self.add_css_class("tm5")
        self.set_default_size(1260, 880)
        self.settings = Settings()
        self.engine = find_engine(engine_hint)
        self.sysinfo: dict = {}
        self.presets: list[dict] = []
        self.user_dir = user_configs_dir()
        self.config: dict | None = None
        self.tests: dict[int, dict] = {}
        self.run: EngineRun | None = None
        self.state = "idle"
        self.inhibit_cookie = 0
        self.close_after_stop = False
        self.lock_warned = False
        self.tick_source = 0
        self._loading = False
        self.temp_cpu: float | None = None
        self.temp_dimms: list = []
        self._temp_poll_busy = False
        self.reset_run_state()
        self.build_ui()
        self.setup_actions()
        self.connect("close-request", self.on_close_request)
        GLib.idle_add(self.load_engine_info)

    # ------------------------------------------------------------------------------------------
    # layout
    # ------------------------------------------------------------------------------------------

    def build_ui(self) -> None:
        header = Gtk.HeaderBar()
        titles = vbox(0)
        titles.set_valign(Gtk.Align.CENTER)
        titles.append(label(APP_NAME, "app-title", xalign=0.5))
        self.subtitle = label("", "app-subtitle", xalign=0.5)
        titles.append(self.subtitle)
        header.set_title_widget(titles)

        menu = Gio.Menu()
        files = Gio.Menu()
        files.append("Load a TM5 config file…", "win.load-config")
        files.append("Open my configs folder", "win.open-configs")
        files.append("Open logs folder", "win.open-logs")
        menu.append_section(None, files)
        help_ = Gio.Menu()
        help_.append("How the tests work", "win.show-guide")
        help_.append("Memory locking help", "win.lock-help")
        help_.append(f"About {APP_NAME}", "app.about")
        menu.append_section(None, help_)
        menu_btn = Gtk.MenuButton(icon_name="open-menu-symbolic", menu_model=menu, tooltip_text="Menu")
        header.pack_end(menu_btn)
        self.set_titlebar(header)

        root = hbox(0)
        self.set_child(root)

        side_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        side_scroll.add_css_class("sidebar")
        side_scroll.set_size_request(340, -1)
        self.sidebar = vbox(22)
        for m in ("top", "bottom", "start", "end"):
            getattr(self.sidebar, f"set_margin_{m}")(20)
        side_scroll.set_child(self.sidebar)
        root.append(side_scroll)
        self.build_sidebar()

        main_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER, hexpand=True)
        main = vbox(16)
        for m in ("top", "bottom", "start", "end"):
            getattr(main, f"set_margin_{m}")(22)
        main_scroll.set_child(main)
        root.append(main_scroll)
        self.build_main(main)

    def section(self, title: str) -> Gtk.Box:
        box = vbox(8)
        box.append(label(title.upper(), "section-title"))
        self.sidebar.append(box)
        return box

    def build_sidebar(self) -> None:
        # Profile
        sec = self.section("Test profile")
        self.preset_model = Gtk.StringList()
        self.preset_dd = Gtk.DropDown(model=self.preset_model)
        self.preset_dd.connect("notify::selected", self.on_preset_changed)
        sec.append(self.preset_dd)
        self.profile_info = label("", "caption", "strong", wrap=True)
        self.profile_note = label("", "caption", wrap=True)
        sec.append(self.profile_info)
        sec.append(self.profile_note)
        load = Gtk.Button(label="Load a TM5 .cfg file…")
        load.set_action_name("win.load-config")
        load.set_halign(Gtk.Align.START)
        sec.append(load)

        # Memory
        sec = self.section("Memory to test")
        self.mem_auto = Gtk.CheckButton(label="Automatic (recommended)")
        self.mem_custom = Gtk.CheckButton(label="Custom amount", group=self.mem_auto)
        self.mem_auto_caption = label("", "caption", wrap=True)
        self.mem_spin = Gtk.SpinButton.new_with_range(64, 64 * 1024 * 1024, 256)
        self.mem_spin.set_digits(0)
        self.mem_spin.set_hexpand(True)
        mem_row = hbox(8)
        mem_row.append(self.mem_spin)
        mem_row.append(label("MiB", "caption"))
        self.mem_custom.connect("toggled", self.on_mem_mode)
        sec.append(self.mem_auto)
        sec.append(self.mem_auto_caption)
        sec.append(self.mem_custom)
        sec.append(mem_row)

        # Threads and cycles
        sec = self.section("CPU threads")
        self.threads_spin = Gtk.SpinButton.new_with_range(1, 4096, 1)
        self.threads_spin.connect("value-changed", self.on_threads_changed)
        self.threads_caption = label("", "caption", wrap=True)
        sec.append(self.threads_spin)
        sec.append(self.threads_caption)

        sec = self.section("Cycles")
        self.cycles_spin = Gtk.SpinButton.new_with_range(0, 9999, 1)
        self.cycles_spin.connect("value-changed", self.on_cycles_changed)
        sec.append(self.cycles_spin)
        sec.append(label("How often the whole test sequence runs. 0 = keep going until you press Stop.",
                         "caption", wrap=True))

        sec = self.section("Test duration")
        self.duration_dd = Gtk.DropDown.new_from_strings([d[1] for d in DURATIONS])
        sec.append(self.duration_dd)
        sec.append(label("Scales how long every test runs. Longer runs catch rarer errors.", "caption", wrap=True))

        sec = self.section("Options")
        self.stop_on_error_cb = Gtk.CheckButton(label="Stop at the first error")
        sec.append(self.stop_on_error_cb)
        self.pkexec_cb = Gtk.CheckButton(label="Run with administrator rights")
        sec.append(self.pkexec_cb)
        self.pkexec_caption = label("Asks for your password. Only needed when memory locking is not available.",
                                    "caption", wrap=True)
        sec.append(self.pkexec_caption)
        if not shutil.which("pkexec"):
            self.pkexec_cb.set_sensitive(False)
            self.pkexec_caption.set_label("Not available: pkexec (polkit) is not installed.")

        sec = self.section("This computer")
        self.sys_cpu = label("", "caption", wrap=True)
        self.sys_mem = label("", "caption", wrap=True)
        sec.append(self.sys_cpu)
        sec.append(self.sys_mem)
        lock_row = hbox(8)
        lock_row.append(label("Memory locking", "caption"))
        self.sys_lock = label("", "pill")
        lock_row.append(self.sys_lock)
        sec.append(lock_row)
        self.lock_note = vbox(8, "note")
        self.lock_note.append(label("Memory cannot be locked, so Linux could move tested memory to swap and errors "
                                    "might be missed.", "caption", wrap=True))
        fix = Gtk.Button(label="How to fix this")
        fix.set_action_name("win.lock-help")
        fix.set_halign(Gtk.Align.START)
        self.lock_note.append(fix)
        self.lock_note.set_visible(False)
        sec.append(self.lock_note)

        self.controls = [self.preset_dd, self.mem_auto, self.mem_custom, self.mem_spin, self.threads_spin,
                         self.cycles_spin, self.duration_dd, self.stop_on_error_cb, self.pkexec_cb, load]

    def build_main(self, main: Gtk.Box) -> None:
        self.banner = Banner()
        self.banner.button.connect("clicked", self.on_main_button)
        main.append(self.banner)

        self.st_elapsed = StatCard("Elapsed", "0:00:00", "not started")
        self.st_remaining = StatCard("Remaining", "–", "shown once testing starts")
        self.st_cycle = StatCard("Cycle", "–", "")
        self.st_errors = StatCard("Errors", "0", "none found",
                                  "Any error means the memory returned wrong data: the settings are not stable.")
        self.st_memory = StatCard("Memory", "–", "")
        self.st_speed = StatCard("Speed", "–", "memory throughput")
        self.st_cpu_temp = StatCard("CPU temp", "–", "reading sensors…")
        self.st_ram_temp = StatCard("RAM temp", "–", "")
        self.st_threads = StatCard("Threads", "–", "")
        # The last card shows the RAM temperature when the modules have sensors, else the thread count.
        self.slot_ram = Gtk.Stack()
        self.slot_ram.add_named(self.st_threads, "threads")
        self.slot_ram.add_named(self.st_ram_temp, "ram")
        self.slot_ram.set_visible_child_name("threads")
        stats = Gtk.Grid(column_homogeneous=True, row_spacing=12, column_spacing=12)
        cards = (self.st_elapsed, self.st_remaining, self.st_cycle, self.st_errors, self.st_memory,
                 self.st_speed, self.st_cpu_temp, self.slot_ram)
        for i, card in enumerate(cards):
            stats.attach(card, i % 4, i // 4, 1, 1)
        main.append(stats)

        bars = vbox(14, "card")
        self.bar_overall = LabeledBar("Overall progress")
        self.bar_test = LabeledBar("Current test")
        bars.append(self.bar_overall)
        bars.append(self.bar_test)
        main.append(bars)

        self.sequence = SequenceView()
        main.append(self.sequence)

        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE)
        self.stack.set_size_request(-1, 300)
        self.log = LogView()
        self.errors = ErrorList()
        self.threads = ThreadsView()
        threads_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        threads_scroll.set_child(self.threads)
        self.guide = vbox(10)
        for m in ("top", "bottom", "start", "end"):
            getattr(self.guide, f"set_margin_{m}")(18)
        guide_scroll = Gtk.ScrolledWindow(hscrollbar_policy=Gtk.PolicyType.NEVER)
        guide_scroll.set_child(self.guide)
        self.stack.add_titled(self.log, "activity", "Activity")
        self.errors_page = self.stack.add_titled(self.errors, "errors", "Errors")
        self.stack.add_titled(threads_scroll, "threads", "Threads")
        self.stack.add_titled(guide_scroll, "guide", "How it works")
        switcher = Gtk.StackSwitcher(stack=self.stack)
        switcher.set_halign(Gtk.Align.START)
        panel = vbox(0, "panel")
        panel.append(self.stack)
        tabs = vbox(10)
        tabs.append(switcher)
        tabs.append(panel)
        main.append(tabs)

    def setup_actions(self) -> None:
        for name, cb in (("load-config", self.on_load_config), ("open-configs", self.on_open_configs),
                         ("open-logs", self.on_open_logs), ("show-guide", self.on_show_guide),
                         ("lock-help", self.on_lock_help)):
            act = Gio.SimpleAction.new(name, None)
            act.connect("activate", cb)
            self.add_action(act)

    # ------------------------------------------------------------------------------------------
    # engine information
    # ------------------------------------------------------------------------------------------

    def load_engine_info(self) -> bool:
        if not self.engine:
            self.banner.set_state("error", "Test engine not found",
                                  "TM5 for Linux could not find its test engine (tm5linux-cli). Please reinstall the "
                                  "app - see the README for instructions.")
            self.banner.set_button("Start test", "start", False)
            return False
        self.sysinfo = first(query(self.engine, "--sysinfo"), "sysinfo") or {}
        self.reload_presets(select=self.settings["preset"])
        self.fill_system_info()
        self.apply_temps(self.sysinfo)
        if self.sysinfo.get("cpu_temp") is not None or self.sysinfo.get("dimm_temps"):
            GLib.timeout_add_seconds(3, self.poll_temps)
        self.apply_settings()
        self.set_idle()
        self.log.add("info", f"{APP_NAME} {VERSION} ready. Test engine: {self.engine}")
        if self.sysinfo and not self.sysinfo.get("can_lock"):
            self.log.add("warn", "Memory locking is not available: " + self.sysinfo.get("lock_reason", "") + ".")
        return False

    def reload_presets(self, select: str | None = None) -> None:
        p = first(query(self.engine, "--list-presets"), "presets") or {}
        self.presets = p.get("presets", [])
        self.user_dir = p.get("user_dir") or self.user_dir
        self._loading = True
        self.preset_model.splice(0, self.preset_model.get_n_items(),
                                 [f"{x['name']} - {x['author']}" + ("  (yours)" if x.get("user") else "")
                                  for x in self.presets])
        idx = 0
        for i, x in enumerate(self.presets):
            if select and select.lower() in (x["id"].lower(), x["alias"].lower(), x["path"]):
                idx = i
                break
        self._loading = False
        self.preset_dd.set_selected(idx)
        self.on_preset_changed()

    def current_preset(self) -> dict | None:
        i = self.preset_dd.get_selected()
        return self.presets[i] if 0 <= i < len(self.presets) else None

    @staticmethod
    def preset_arg(p: dict) -> str:
        return p["alias"] if p["path"] == "built-in" else p["path"]

    def on_preset_changed(self, *_args) -> None:
        if self._loading:
            return
        p = self.current_preset()
        if not p:
            return
        cfg = first(query(self.engine, "--describe", "--config", self.preset_arg(p)), "config")
        if not cfg:
            self.profile_info.set_label("This config could not be read.")
            return
        self.config = cfg
        self.tests = {t["index"]: t for t in cfg["tests"]}
        seq = cfg["sequence"]
        self.profile_info.set_label(f"{len(seq)} steps per cycle · {len(set(seq))} different tests · "
                                    f"{cfg['cycles']} cycles")
        note = PROFILE_NOTES.get(p["alias"].lower(), "Your own TM5 config." if p.get("user") else "")
        if cfg.get("warnings"):
            note += ("\n" if note else "") + "Note: " + " ".join(cfg["warnings"])
        self.profile_note.set_label(note)
        self.profile_note.set_visible(bool(note))
        self.sequence.set_sequence(seq, self.tests, cfg["cycles"])
        self.cycles_spin.set_value(cfg["cycles"])
        self.sequence.title.set_label("Test sequence")
        self.subtitle.set_label(f"{cfg['name']} by {cfg['author']}")
        self.build_guide()

    def fill_system_info(self) -> None:
        si = self.sysinfo
        if not si:
            return
        self.sys_cpu.set_label(f"{si.get('cpu', 'Unknown CPU')} · {si.get('cpus', '?')} threads")
        self.sys_mem.set_label(f"{fmt_bytes(si.get('mem_total', 0))} RAM, {fmt_bytes(si.get('mem_available', 0))} free"
                               f" · huge pages: {si.get('thp', '?')}")
        can = si.get("can_lock")
        self.sys_lock.set_label("available ✔" if can else "not available")
        set_tone(self.sys_lock, "good" if can else "warn")
        self.sys_lock.set_tooltip_text(si.get("lock_reason", ""))
        self.lock_note.set_visible(not can)
        default = si.get("default_bytes", 0)
        keep = max(0, si.get("mem_available", 0) - default)
        self.mem_auto_caption.set_label(f"Tests {fmt_bytes(default)}: all free memory except {fmt_bytes(keep)} kept "
                                        f"for your system.")
        self.mem_spin.set_range(64, max(64, si.get("mem_available", 0) // (1 << 20) - 256))
        cpus = int(si.get("cpus", 1))
        self.threads_spin.set_range(1, max(1, cpus))

    def apply_settings(self) -> None:
        s = self.settings
        (self.mem_custom if s["memory_mode"] == "custom" else self.mem_auto).set_active(True)
        self.mem_spin.set_value(float(s["memory_mb"]))
        self.on_mem_mode()
        cpus = int(self.sysinfo.get("cpus", 1))
        self.threads_spin.set_value(s["threads"] if 0 < int(s["threads"] or 0) <= cpus else cpus)
        self.on_threads_changed()
        scale = int(s["time_scale"] or 100)
        self.duration_dd.set_selected(next((i for i, d in enumerate(DURATIONS) if d[0] == scale), 2))
        self.stop_on_error_cb.set_active(bool(s["stop_on_error"]))
        self.pkexec_cb.set_active(bool(s["use_pkexec"]) and self.pkexec_cb.get_sensitive())
        if s["cycles"] is not None:
            self.cycles_spin.set_value(int(s["cycles"]))

    def save_settings(self) -> None:
        s = self.settings
        p = self.current_preset()
        if p:
            s["preset"] = p["id"]
        s["memory_mode"] = "custom" if self.mem_custom.get_active() else "auto"
        s["memory_mb"] = int(self.mem_spin.get_value())
        cpus = int(self.sysinfo.get("cpus", 1))
        t = int(self.threads_spin.get_value())
        s["threads"] = 0 if t >= cpus else t
        cyc = int(self.cycles_spin.get_value())
        s["cycles"] = None if self.config and cyc == self.config.get("cycles") else cyc
        s["time_scale"] = DURATIONS[self.duration_dd.get_selected()][0]
        s["stop_on_error"] = self.stop_on_error_cb.get_active()
        s["use_pkexec"] = self.pkexec_cb.get_active()
        s.save()

    def on_mem_mode(self, *_args) -> None:
        self.mem_spin.set_sensitive(self.mem_custom.get_active() and self.state not in ("preparing", "running"))

    def on_cycles_changed(self, *_args) -> None:
        if self.state not in ("preparing", "running"):
            self.sequence.set_cycles(int(self.cycles_spin.get_value()))

    def on_threads_changed(self, *_args) -> None:
        cpus = int(self.sysinfo.get("cpus", 1))
        t = int(self.threads_spin.get_value())
        self.threads_caption.set_label(f"All {cpus} threads (recommended)" if t >= cpus else
                                       f"{t} of {cpus} threads - fewer threads put less load on the memory.")

    def build_guide(self) -> None:
        child = self.guide.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.guide.remove(child)
            child = nxt
        g = self.guide

        def heading(text: str) -> None:
            w = label(text, "guide-heading")
            w.set_margin_top(8)
            g.append(w)

        def para(text: str) -> None:
            g.append(label(text, "caption", wrap=True))

        heading("How a test works")
        para("1.  TM5 for Linux reserves almost all free memory and locks it, so Linux cannot move it to swap.")
        para("2.  Every CPU thread gets its own slice of that memory, and all threads run the same test at the "
             "same time - just like TM5 on Windows.")
        para("3.  Each test writes known data, reads it back and compares every single bit. Any difference is "
             "counted as an error.")
        para("4.  The profile runs its tests in a fixed order (the sequence) and repeats that sequence for "
             "several cycles. The test numbers in the sequence are the test numbers of the TM5 config.")

        if self.config:
            cfg = self.config
            heading(f"The tests in {cfg['name']} by {cfg['author']}")
            seen: list[int] = []
            for idx in cfg["sequence"]:
                if idx not in seen:
                    seen.append(idx)
            for idx in seen:
                t = self.tests.get(idx, {})
                uses = cfg["sequence"].count(idx)
                row = hbox(12)
                num = label(f"#{idx}", "test-num")
                num.set_size_request(36, -1)
                num.set_valign(Gtk.Align.START)
                col = vbox(2)
                col.set_hexpand(True)
                col.append(label(f"{t.get('function', '?')}  ·  {t.get('summary', '')}", "strong", wrap=True))
                col.append(label(t.get("explain", ""), "caption", wrap=True))
                col.append(label(f"Runs {uses}× per cycle, {t.get('passes', '?')} passes each time.", "dim",
                                 wrap=True))
                row.append(num)
                row.append(col)
                row.set_margin_bottom(6)
                g.append(row)

        heading("Reading the result")
        para("PASSED - not a single bit was wrong during the whole run. For overclocked memory, let the full "
             "number of cycles finish before you trust the settings.")
        para("FAILED - the memory returned wrong data at least once. Even one error can crash programs or "
             "silently corrupt files, so the settings are not stable.")
        para("\"Read error\" means reading the same address again gave the right value - typically timings that "
             "are too tight, too little voltage or a frequency that is too high. \"Wrong value stored in RAM\" "
             "means the bits really flipped - often refresh timings, weak cells or heat.")
        heading("Tips")
        para("•  Close other programs first and do not use the computer heavily while testing.")
        para("•  Errors that only show up after a long time are often caused by heat - make sure your memory "
             "has some airflow. The CPU temperature is always shown; the RAM temperature appears when your "
             "modules have a sensor (most DDR5 modules do, on Linux 6.11 or newer).")
        para("•  At stock settings (XMP/EXPO off), errors can mean a faulty module: test the modules one at a "
             "time.")
        para("•  The computer is kept awake while a test runs.")

    # ------------------------------------------------------------------------------------------
    # run state & UI updates
    # ------------------------------------------------------------------------------------------

    def reset_run_state(self) -> None:
        self.finish_event: dict | None = None
        self.total_errors = 0
        self.cycles = 0
        self.steps = 0
        self.cycle = 0
        self.step = -1
        self.start_mono = 0.0
        self.eta = None
        self.eta_mono = 0.0
        self.mem_bytes = 0
        self.locked = None
        self.stderr_tail: list[str] = []
        self.log_path: str | None = None
        self.errors_by_test: dict[int, int] = {}
        self.run_threads = 0
        self.run_cpu_max: float | None = None
        self.run_dimm_max: float | None = None

    def set_controls_sensitive(self, on: bool) -> None:
        for w in self.controls:
            w.set_sensitive(on)
        if not shutil.which("pkexec"):
            self.pkexec_cb.set_sensitive(False)
        self.on_mem_mode()

    def set_idle(self) -> None:
        self.state = "idle"
        self.banner.set_state("idle", "Ready to test",
                              "Pick a profile and press Start. Close other programs first - the test uses almost "
                              "all free memory and keeps every CPU thread busy.")
        self.banner.set_button("Start test", "start", bool(self.config))

    def update_errors_card(self) -> None:
        n = self.total_errors
        if n:
            tests = ", ".join(f"#{t}" for t in sorted(self.errors_by_test))
            self.st_errors.set(str(n), f"in test {tests}", "bad")
        else:
            self.st_errors.set("0", "none found", "good" if self.state in ("running", "pass") else None)
        self.errors_page.set_title(f"Errors ({n})" if n else "Errors")

    def on_tick(self) -> bool:
        if self.state not in ("preparing", "running"):
            self.tick_source = 0
            return False
        now = time.monotonic()
        self.st_elapsed.set(fmt_duration(now - self.start_mono), "since start")
        if self.eta is not None:
            left = max(0.0, self.eta - (now - self.eta_mono))
            self.st_remaining.set(fmt_eta(left), "estimated, gets better over time")
        return True

    # ------------------------------------------------------------------------------------------
    # starting and stopping
    # ------------------------------------------------------------------------------------------

    def on_main_button(self, *_args) -> None:
        if self.state in ("preparing", "running"):
            self.stop_test()
        else:
            self.start_test()

    def build_args(self) -> list[str]:
        p = self.current_preset()
        args = ["--config", self.preset_arg(p), "--time-scale", str(DURATIONS[self.duration_dd.get_selected()][0]),
                "--cycles", str(int(self.cycles_spin.get_value()))]
        if self.mem_custom.get_active():
            args += ["--memory", f"{int(self.mem_spin.get_value())}M"]
        cpus = int(self.sysinfo.get("cpus", 1))
        t = int(self.threads_spin.get_value())
        if t < cpus:
            args += ["--threads", str(t)]
        if self.stop_on_error_cb.get_active():
            args.append("--stop-on-error")
        return args

    def start_test(self) -> None:
        if not self.engine or not self.config or self.current_preset() is None:
            return
        use_pkexec = self.pkexec_cb.get_active()
        if not self.sysinfo.get("can_lock") and not use_pkexec and not self.lock_warned:
            self.lock_warned = True
            buttons = [("cancel", "Cancel", "")]
            if self.pkexec_cb.get_sensitive():
                buttons.append(("admin", "Run as administrator", ""))
            buttons.append(("run", "Run anyway", "suggested-action"))
            ask(self, "Memory cannot be locked",
                "Without locking, Linux may move parts of the tested memory to swap or zram while the test runs, "
                "and errors in that part could be missed. You can still run the test, run it with administrator "
                "rights, or fix this permanently (see \"Memory locking help\" in the menu).",
                buttons, lambda r: self.really_start(r == "admin") if r in ("run", "admin") else None)
            return
        self.really_start(use_pkexec)

    def really_start(self, use_pkexec: bool) -> None:
        self.save_settings()
        self.reset_run_state()
        self.log.clear()
        self.errors.clear()
        self.sequence.reset()
        self.sequence.set_cycles(int(self.cycles_spin.get_value()))
        self.update_errors_card()
        self.open_logfile()
        args = self.build_args()
        self.run = EngineRun(self.engine, args, self.on_engine_event, self.on_engine_stderr, self.on_engine_exit,
                             use_pkexec=use_pkexec)
        cfg = self.config
        self.log.add("info", f"Starting {cfg['name']} by {cfg['author']}" +
                     (" with administrator rights (you may be asked for your password)" if use_pkexec else "") + ".")
        try:
            self.run.start()
        except OSError as e:
            self.run = None
            self.finish("error", message=f"Could not start the test engine: {e}")
            return
        self.state = "preparing"
        self.start_mono = time.monotonic()
        self.set_controls_sensitive(False)
        self.banner.set_state("preparing", "Preparing memory…",
                              "Reserving the memory to test and locking it in RAM. With a lot of memory this can take "
                              "up to a minute.")
        self.banner.set_button("Stop", "stop")
        self.bar_overall.set(0, "", None)
        self.bar_test.set(0, "", "prep")
        self.st_remaining.set("–", "shown once testing starts")
        self.st_speed.set("–", "memory throughput")
        app = self.get_application()
        if app:
            flags = Gtk.ApplicationInhibitFlags.SUSPEND | Gtk.ApplicationInhibitFlags.IDLE
            self.inhibit_cookie = app.inhibit(self, flags, "A memory test is running")
        if not self.tick_source:
            self.tick_source = GLib.timeout_add(500, self.on_tick)

    def stop_test(self) -> None:
        if self.run and self.run.running:
            self.run.stop()
            self.banner.set_text("Stopping - finishing the current block of memory…")
            self.banner.set_button("Stopping…", "stop", False)
            self.log.add("info", "Stop requested.")

    def open_logfile(self) -> None:
        try:
            os.makedirs(logs_dir(), exist_ok=True)
            self.log_path = os.path.join(logs_dir(), time.strftime("tm5linux-%Y%m%d-%H%M%S.log"))
            f = open(self.log_path, "w", encoding="utf-8")
        except OSError:
            self.log_path = None
            return
        si, cfg = self.sysinfo, self.config or {}
        f.write(f"{APP_NAME} {VERSION} - memory stability test log\n")
        f.write(f"Date   : {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"Config : {cfg.get('name')} by {cfg.get('author')} ({cfg.get('source')})\n")
        f.write(f"CPU    : {si.get('cpu')} ({si.get('cpus')} threads)\n")
        f.write(f"Memory : {fmt_bytes(si.get('mem_total', 0))} total, {fmt_bytes(si.get('mem_available', 0))} free\n")
        f.write(f"Kernel : {si.get('kernel')} {si.get('arch')}\n")
        f.write(f"Temps  : {temps_text(self.temp_cpu, self.dimm_hottest()) or 'no temperature sensors found'}\n")
        f.write(f"Options: {' '.join(self.build_args())}\n\n")
        self.log.file = f

    def close_logfile(self, summary: str) -> None:
        f = self.log.file
        self.log.file = None
        if not f:
            return
        try:
            f.write(f"\nRESULT : {summary}\n")
            f.close()
        except OSError:
            pass

    # ------------------------------------------------------------------------------------------
    # temperatures
    # ------------------------------------------------------------------------------------------

    def poll_temps(self) -> bool:
        """While no test runs, read the sensors every few seconds (during a test the engine reports them)."""
        if self.state in ("preparing", "running") or self._temp_poll_busy:
            return True
        self._temp_poll_busy = True

        def work() -> None:
            ev = first(query(self.engine, "--sensors", timeout=5), "temps")
            GLib.idle_add(self._temps_polled, ev)

        threading.Thread(target=work, daemon=True).start()
        return True

    def _temps_polled(self, ev: dict | None) -> bool:
        self._temp_poll_busy = False
        if ev and self.state not in ("preparing", "running"):
            self.apply_temps(ev)
        return False

    def apply_temps(self, ev: dict) -> None:
        cpu = ev.get("cpu_temp")
        dimms = list(ev.get("dimm_temps") or [])
        self.temp_cpu, self.temp_dimms = cpu, dimms
        if ev.get("cpu_temp_max") is not None:
            self.run_cpu_max = ev["cpu_temp_max"]
        if ev.get("dimm_temp_max") is not None:
            self.run_dimm_max = ev["dimm_temp_max"]
        running = self.state in ("preparing", "running")

        def maxline(m: float | None, fallback: str) -> str:
            if m is None:
                return fallback
            return f"max {fmt_temp(m)}" + ("" if running else " during the test")

        if cpu is None:
            self.st_cpu_temp.set("–", "no sensor found", None)
            self.st_cpu_temp.set_tooltip_text(CPU_NO_SENSOR_TIP)
        else:
            self.st_cpu_temp.set(fmt_temp(cpu), maxline(self.run_cpu_max, ev.get("cpu_sensor") or ""),
                                 temp_tone(cpu, 85, 95))
            self.st_cpu_temp.set_tooltip_text(CPU_TEMP_TIP.format(sensor=ev.get("cpu_sensor") or "unknown sensor"))

        if dimms:
            self.slot_ram.set_visible_child_name("ram")
            known = [d for d in dimms if d is not None]
            hottest = max(known) if known else None
            per = " · ".join("–" if d is None else f"{d:.0f}" for d in dimms) + " °C"
            self.st_ram_temp.set(fmt_temp(hottest),
                                 maxline(self.run_dimm_max, per if len(dimms) > 1 else "1 module"),
                                 temp_tone(hottest, 50, 60))
            modules = "\n".join(f"Module {i + 1}: {fmt_temp(d)}" for i, d in enumerate(dimms))
            self.st_ram_temp.set_tooltip_text(RAM_TEMP_TIP.format(sensor=ev.get("dimm_sensor") or "?",
                                                                  modules=modules))
        else:
            self.slot_ram.set_visible_child_name("threads")

    def dimm_hottest(self) -> float | None:
        known = [d for d in self.temp_dimms if d is not None]
        return max(known) if known else None

    # ------------------------------------------------------------------------------------------
    # engine events
    # ------------------------------------------------------------------------------------------

    def on_engine_event(self, ev: dict) -> bool:
        handler = getattr(self, "ev_" + str(ev.get("type")), None)
        if handler:
            handler(ev)
        return False

    def on_engine_stderr(self, line: str) -> bool:
        self.stderr_tail = (self.stderr_tail + [line])[-6:]
        self.log.add("warn", line)
        return False

    def ev_temps(self, ev: dict) -> None:
        self.apply_temps(ev)

    def ev_sysinfo(self, ev: dict) -> None:
        self.apply_temps(ev)
        if ev.get("root"):
            self.log.add("info", "The test engine is running with administrator rights.")

    def ev_config(self, ev: dict) -> None:
        self.cycles = int(ev.get("cycles", 0))
        self.tests = {t["index"]: t for t in ev.get("tests", [])}
        self.sequence.set_sequence(ev["sequence"], self.tests, self.cycles)
        self.steps = len(ev["sequence"])

    def ev_log(self, ev: dict) -> None:
        self.log.add(ev.get("level", "info"), ev.get("msg", ""))

    def ev_alloc_start(self, ev: dict) -> None:
        self.mem_bytes = int(ev["bytes"])
        threads = int(ev["threads"])
        self.run_threads = threads
        self.st_memory.set(fmt_bytes(self.mem_bytes), f"{threads} threads · preparing…")
        self.st_threads.set(str(threads), f"{fmt_bytes(ev['per_thread'])} each")
        self.threads.setup(threads, fmt_bytes(ev["per_thread"]))
        self.banner.set_text(f"Reserving {fmt_bytes(self.mem_bytes)} of RAM and locking it so it cannot be swapped "
                             f"out. With a lot of memory this can take up to a minute.")
        self.log.add("info", f"Preparing {fmt_bytes(self.mem_bytes)} of memory for {threads} threads.")

    def ev_alloc_progress(self, ev: dict) -> None:
        self.bar_test.title.set_label("Preparing memory")
        self.bar_test.set(float(ev.get("pct", 0)), "", "prep")

    def ev_alloc_done(self, ev: dict) -> None:
        self.locked = bool(ev.get("locked"))
        huge = float(ev.get("huge_pct") or 0)
        self.st_memory.set(fmt_bytes(self.mem_bytes),
                           f"{self.run_threads} threads · " + ("locked ✔" if self.locked else "not locked ▲"), None)
        set_tone(self.st_memory.sub, "good" if self.locked else "warn")
        self.log.add("ok" if self.locked else "warn",
                     f"Memory ready in {float(ev.get('seconds', 0)):.1f} s: {fmt_bytes(self.mem_bytes)} "
                     f"{'locked in RAM' if self.locked else 'NOT locked'}, {huge:.0f} % in huge pages.")
        temps = temps_text(self.temp_cpu, self.dimm_hottest())
        if temps:
            self.log.add("info", f"Temperatures at the start: {temps}.")
        self.state = "running"
        self.bar_test.title.set_label("Current test")

    def ev_test_start(self, ev: dict) -> None:
        cycle, step, ti = int(ev["cycle"]), int(ev["step"]), int(ev["test"])
        cycles, steps = int(ev.get("cycles", self.cycles)), int(ev.get("steps", self.steps))
        if cycle != self.cycle:
            self.sequence.reset()
            self.sequence.mark_cycle(cycle, "running")
        self.cycle, self.step, self.cycles, self.steps = cycle, step, cycles, steps
        self.sequence.mark(step, "running")
        self.sequence.title.set_label(f"Test sequence - cycle {cycle}" + (f" of {cycles}" if cycles else ""))
        self.st_cycle.set(f"{cycle} / {cycles}" if cycles else str(cycle),
                          f"step {step + 1} of {steps}")
        self.bar_test.set(0, f"test #{ti} · {ev.get('summary', '')}", "fail" if self.total_errors else None)
        cyc = f"cycle {cycle} of {cycles}" if cycles else f"cycle {cycle}"
        detail = f"Step {step + 1} of {steps}  ·  Test #{ti} {ev.get('function')}  ·  {ev.get('summary', '')}"
        if self.total_errors:
            self.banner.set_state("fail", f"Errors found - {cyc}", ev.get("explain", ""), detail)
        else:
            self.banner.set_state("running", f"Testing - {cyc}", ev.get("explain", ""), detail)
        self.log.add("info", f"Step {step + 1}/{steps}: test #{ti} {ev.get('function')} - {ev.get('summary', '')}")

    def ev_progress(self, ev: dict) -> None:
        now = time.monotonic()
        eta = ev.get("eta")
        self.eta = float(eta) if eta is not None else None
        self.eta_mono = now
        tone = "fail" if self.total_errors else None
        if self.cycles:
            self.bar_overall.set(float(ev.get("run_pct", 0)), fmt_eta(self.eta) + " left" if self.eta else "",
                                 tone or "ok")
        else:
            self.bar_overall.set(0, "runs until you press Stop", tone)
        self.bar_test.set(float(ev.get("test_pct", 0)), None, tone)
        self.st_speed.set(fmt_speed(ev.get("speed")), "memory throughput")
        if int(ev.get("errors", 0)) > self.total_errors:
            self.total_errors = int(ev["errors"])
            self.update_errors_card()
        self.st_remaining.set(fmt_eta(self.eta) if self.cycles else "∞",
                              "estimated, gets better over time" if self.cycles else "until you press Stop")
        self.threads.update(ev.get("threads", []), ev.get("thread_errors", []))

    def ev_error(self, ev: dict) -> None:
        first_error = self.total_errors == 0
        self.total_errors = max(self.total_errors, int(ev.get("n", 0)))
        ti = int(ev.get("test", -1))
        self.errors_by_test.setdefault(ti, 0)
        self.errors.add(ev)
        self.sequence.mark(int(ev.get("step", -1)), "fail")
        self.update_errors_card()
        bits = int(ev.get("bits", 0))
        self.log.add("error", f"ERROR in test #{ti} (thread {ev.get('thread')}): expected {ev.get('expected')}, "
                              f"read {ev.get('actual')} - {bits} bit{'s' if bits != 1 else ''} wrong "
                              f"(bit {ev.get('bit_list')}), "
                              f"{'wrong value stored in RAM' if ev.get('kind') == 'persistent' else 'read error'}"
                              + (f" ({temps})." if (temps := temps_text(ev.get("cpu_temp"), ev.get("dimm_temp")))
                                 else "."))
        if first_error:
            cyc = f"cycle {self.cycle} of {self.cycles}" if self.cycles else f"cycle {self.cycle}"
            self.banner.set_state("fail", f"Errors found - {cyc}", self.banner.text.get_label(),
                                  self.banner.detail.get_label())
            self.stack.set_visible_child_name("errors")

    def ev_test_end(self, ev: dict) -> None:
        step, ti, errs = int(ev["step"]), int(ev["test"]), int(ev.get("errors", 0))
        secs = float(ev.get("seconds", 0))
        took = f"{secs:.1f} s" if secs < 60 else fmt_duration(secs)
        if errs:
            self.errors_by_test[ti] = self.errors_by_test.get(ti, 0) + errs
            self.update_errors_card()
            self.sequence.mark(step, "fail")
            self.log.add("error", f"Test #{ti} found {errs} error{'s' if errs != 1 else ''} (took {took}).")
        elif ev.get("completed", True):
            self.sequence.mark(step, "pass")
            self.threads.complete()
            self.log.add("ok", f"Test #{ti} passed in {took}.")
        else:
            self.sequence.mark(step, "")

    def ev_cycle_end(self, ev: dict) -> None:
        c = int(ev["cycle"])
        errs = int(ev.get("errors", 0))
        self.sequence.mark_cycle(c, "fail" if errs else "pass")
        total = int(ev.get("total_errors", 0))
        of = f" of {self.cycles}" if self.cycles else ""
        hot = temps_text(self.run_cpu_max, self.run_dimm_max)
        self.log.add("error" if total else "ok",
                     f"Cycle {c}{of} finished - {total} error{'s' if total != 1 else ''} so far."
                     + (f" Highest temperatures so far: {hot}." if hot else ""))

    def ev_finish(self, ev: dict) -> None:
        self.finish_event = ev
        self.finish(ev.get("result", "error"), ev)

    def on_engine_exit(self, code: int) -> None:
        if self.finish_event is None:
            if self.run and self.run.use_pkexec and code in (126, 127):
                msg = "Administrator authentication was cancelled or refused, so the test did not start."
            else:
                msg = f"The test engine stopped unexpectedly (exit status {code})."
                if self.stderr_tail:
                    msg += " Last message: " + self.stderr_tail[-1]
                if code in (-9, 137):
                    msg += " It was probably killed because the system ran out of memory - try testing less memory."
            self.finish("error", {"message": msg})
        self.run = None
        if self.close_after_stop:
            self.close()

    # ------------------------------------------------------------------------------------------
    # result
    # ------------------------------------------------------------------------------------------

    def finish(self, result: str, ev: dict | None = None) -> None:
        if self.state in ("pass", "fail", "stopped", "error"):
            return
        ev = ev or {}
        self.state = result if result in ("pass", "fail", "stopped") else "error"
        elapsed = float(ev.get("elapsed", time.monotonic() - self.start_mono if self.start_mono else 0))
        dur = fmt_duration(elapsed)
        cfg = self.config or {}
        name = f"{cfg.get('name', '?')} by {cfg.get('author', '?')}"
        errors = int(ev.get("errors", self.total_errors))
        self.total_errors = max(self.total_errors, errors)
        cycles_done = int(ev.get("cycles_done", 0))
        by_test = ev.get("errors_by_test") or {str(k): v for k, v in self.errors_by_test.items()}

        if self.state == "pass":
            title = "Passed - no errors found"
            text = (f"{cycles_done} cycle{'s' if cycles_done != 1 else ''} of {name} finished in {dur} without a "
                    f"single error. If you changed memory settings, this is a good sign they are stable.")
            summary = f"PASSED - no errors in {cycles_done} cycle(s), {dur}"
            self.bar_overall.set(100, "done", "ok")
            self.bar_test.set(100, "", "ok")
        elif self.state == "fail":
            tests = ", ".join(f"#{k}" for k in sorted(by_test, key=lambda x: int(x)))
            title = f"Failed - {errors} error{'s' if errors != 1 else ''} found"
            text = (f"Your memory returned wrong data in test {tests}, so it is not stable at the current settings. "
                    f"If you are overclocking: loosen timings, lower the frequency or adjust voltages, then test "
                    f"again. At stock settings, test one module at a time to find a faulty one.")
            summary = f"FAILED - {errors} error(s) in test {tests}, after {dur}"
        elif self.state == "stopped":
            title = "Stopped"
            text = (f"You stopped the test after {dur}. No errors were found up to that point, but an unfinished "
                    f"run does not prove the memory is stable.")
            summary = f"STOPPED after {dur} - no errors so far"
        else:
            title = "The test could not run"
            text = ev.get("message") or "See the activity log for details."
            summary = "ERROR - " + text
        hot = temps_text(self.run_cpu_max, self.run_dimm_max)
        if hot and self.state in ("pass", "fail", "stopped"):
            text += f" Highest temperatures: {hot}."
            summary += f" (highest temperatures: {hot})"
        completed = cycles_done > 0 and (not self.cycles or cycles_done >= self.cycles)
        if self.state == "fail":
            self.bar_overall.set(100 if completed else self.bar_overall.bar.get_fraction() * 100,
                                 "done" if completed else "stopped", "fail")
            if completed:
                self.bar_test.set(100, "", "fail")
        elif self.state in ("stopped", "error"):
            self.bar_overall.detail.set_label("stopped" if self.state == "stopped" else "")
        self.banner.set_state(self.state, title, text)
        self.banner.set_button("Run again" if self.state != "error" else "Try again", "start")
        self.st_remaining.set("–", "finished")
        self.st_elapsed.set(dur, "total time")
        self.apply_temps({"cpu_temp": self.temp_cpu, "dimm_temps": self.temp_dimms,
                          "cpu_sensor": self.sysinfo.get("cpu_sensor"), "dimm_sensor": self.sysinfo.get("dimm_sensor")})
        self.update_errors_card()
        self.log.add({"pass": "ok", "fail": "error", "stopped": "warn"}.get(self.state, "error"), summary)
        if self.log_path:
            self.log.add("info", f"Log saved to {self.log_path}")
        self.close_logfile(summary)
        self.set_controls_sensitive(True)
        app = self.get_application()
        if app and self.inhibit_cookie:
            app.uninhibit(self.inhibit_cookie)
            self.inhibit_cookie = 0
        if app and self.state in ("pass", "fail"):
            n = Gio.Notification.new(f"Memory test {'passed' if self.state == 'pass' else 'FAILED'}")
            n.set_body(text)
            app.send_notification("result", n)

    # ------------------------------------------------------------------------------------------
    # menu actions
    # ------------------------------------------------------------------------------------------

    def on_close_request(self, *_args) -> bool:
        if self.run and self.run.running:
            if self.close_after_stop:
                return True

            def answer(rid: str) -> None:
                if rid == "quit":
                    self.close_after_stop = True
                    self.stop_test()

            ask(self, "Stop the running test?", "A memory test is still running. Do you want to stop it and quit?",
                [("cancel", "Keep testing", ""), ("quit", "Stop and quit", "destructive-action")], answer)
            return True
        self.save_settings()
        return False

    def open_folder(self, path: str) -> None:
        os.makedirs(path, exist_ok=True)
        try:
            Gio.AppInfo.launch_default_for_uri(GLib.filename_to_uri(path, None), None)
        except GLib.Error:
            ask(self, "Folder location", "Could not open a file manager. The folder is here:",
                [("ok", "Close", "")], None, command=path)

    def on_open_logs(self, *_args) -> None:
        self.open_folder(logs_dir())

    def on_open_configs(self, *_args) -> None:
        self.open_folder(self.user_dir)

    def on_show_guide(self, *_args) -> None:
        self.stack.set_visible_child_name("guide")

    def on_lock_help(self, *_args) -> None:
        engine = self.engine or "/usr/local/bin/tm5linux-cli"
        can = self.sysinfo.get("can_lock")
        status = ("Memory locking is available on this computer (" + self.sysinfo.get("lock_reason", "") + "). "
                  if can else "Memory locking is NOT available: " + self.sysinfo.get("lock_reason", "") + ". ")
        ask(self, "Memory locking",
            status + "Locking keeps the tested memory in RAM so Linux cannot swap it out during the test. The "
            "installer normally allows this by giving the test engine the CAP_IPC_LOCK capability. If that was "
            "skipped, run this command once in a terminal (it asks for your password), then restart the app. "
            "Alternatively, tick \"Run with administrator rights\".",
            [("ok", "Close", "")], None, command=f"sudo setcap cap_ipc_lock=ep {engine}")

    def on_load_config(self, *_args) -> None:
        if self.state in ("preparing", "running"):
            return
        cfg_filter = Gtk.FileFilter()
        cfg_filter.set_name("TM5 config files (*.cfg)")
        cfg_filter.add_pattern("*.cfg")
        cfg_filter.add_pattern("*.CFG")
        if hasattr(Gtk, "FileDialog"):
            dlg = Gtk.FileDialog(title="Load a TM5 config file")
            filters = Gio.ListStore.new(Gtk.FileFilter)
            filters.append(cfg_filter)
            dlg.set_filters(filters)

            def done(d, res):
                try:
                    f = d.open_finish(res)
                except GLib.Error:
                    return
                if f and f.get_path():
                    self.import_config(f.get_path())

            dlg.open(self, None, done)
        else:
            native = Gtk.FileChooserNative(title="Load a TM5 config file", transient_for=self,
                                           action=Gtk.FileChooserAction.OPEN)
            native.add_filter(cfg_filter)

            def response(n, rid):
                if rid == Gtk.ResponseType.ACCEPT:
                    f = n.get_file()
                    if f and f.get_path():
                        self.import_config(f.get_path())
                self._native = None

            native.connect("response", response)
            self._native = native
            native.show()

    def import_config(self, path: str) -> None:
        events = query(self.engine, "--describe", "--config", path)
        cfg = first(events, "config")
        if not cfg:
            fin = first(events, "finish") or {}
            ask(self, "This file could not be loaded", fin.get("message", "It does not look like a TM5 config file."),
                [("ok", "Close", "")], None)
            return
        os.makedirs(self.user_dir, exist_ok=True)
        dest = os.path.join(self.user_dir, os.path.basename(path))
        if not dest.lower().endswith(".cfg"):
            dest += ".cfg"
        if os.path.realpath(path) != os.path.realpath(dest):
            shutil.copyfile(path, dest)
        self.reload_presets(select=os.path.basename(dest)[:-4])
        self.log.add("ok", f"Loaded {cfg['name']} by {cfg['author']} (copied to {dest}).")
