"""Reusable widgets of the main window."""
from __future__ import annotations

from gi.repository import GLib, Gtk, Pango

from .util import clock, fmt_duration

TONES = ("good", "bad", "warn", "accent")


def label(text: str = "", *classes: str, xalign: float = 0.0, wrap: bool = False, selectable: bool = False,
          ellipsize: bool = False) -> Gtk.Label:
    w = Gtk.Label(label=text, xalign=xalign)
    for c in classes:
        w.add_css_class(c)
    if wrap:
        w.set_wrap(True)
        w.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
    if ellipsize:
        w.set_ellipsize(Pango.EllipsizeMode.END)
    w.set_selectable(selectable)
    return w


def set_tone(widget: Gtk.Widget, tone: str | None) -> None:
    for t in TONES:
        widget.remove_css_class(t)
    if tone:
        widget.add_css_class(tone)


def vbox(spacing: int = 0, *classes: str) -> Gtk.Box:
    b = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=spacing)
    for c in classes:
        b.add_css_class(c)
    return b


def hbox(spacing: int = 0, *classes: str) -> Gtk.Box:
    b = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=spacing)
    for c in classes:
        b.add_css_class(c)
    return b


class StatCard(Gtk.Box):
    """A small tile: LABEL / big value / sub-line."""

    def __init__(self, title: str, value: str = "–", sub: str = "", tooltip: str = ""):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        self.add_css_class("stat")
        self.title = label(title.upper(), "stat-label")
        self.value = label(value, "stat-value", ellipsize=True)
        self.sub = label(sub, "stat-sub", ellipsize=True)
        for w in (self.title, self.value, self.sub):
            self.append(w)
        if tooltip:
            self.set_tooltip_text(tooltip)

    def set(self, value: str, sub: str | None = None, tone: str | None = None) -> None:
        self.value.set_label(value)
        if sub is not None:
            self.sub.set_label(sub)
        set_tone(self.value, tone)


class Banner(Gtk.Box):
    """The big status message at the top: icon, title, plain-language explanation, main button."""

    STATES = ("idle", "preparing", "running", "pass", "fail", "stopped", "error")
    ICONS = {"idle": "▶", "preparing": "◔", "running": "●", "pass": "✔", "fail": "✘", "stopped": "■", "error": "!"}
    TONES = {"idle": "accent", "preparing": "warn", "running": "accent", "pass": "good", "fail": "bad",
             "stopped": None, "error": "bad"}

    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        self.add_css_class("banner")
        self.icon = label("▶", "banner-icon", xalign=0.5)
        self.icon.set_valign(Gtk.Align.CENTER)
        texts = vbox(4)
        texts.set_hexpand(True)
        texts.set_valign(Gtk.Align.CENTER)
        self.title = label("", "banner-title", wrap=True)
        self.detail = label("", "banner-detail", wrap=True)
        self.detail.set_visible(False)
        self.text = label("", "banner-text", wrap=True)
        texts.append(self.title)
        texts.append(self.detail)
        texts.append(self.text)
        self.button = Gtk.Button()
        self.button.add_css_class("action")
        self.button.set_valign(Gtk.Align.CENTER)
        self.append(self.icon)
        self.append(texts)
        self.append(self.button)

    def set_state(self, state: str, title: str, text: str, detail: str = "", tone: str | None = None) -> None:
        for s in self.STATES:
            self.remove_css_class(s)
        self.add_css_class(state)
        self.icon.set_label(self.ICONS.get(state, "●"))
        set_tone(self.icon, tone if tone is not None else self.TONES.get(state))
        self.title.set_label(title)
        self.set_detail(detail)
        self.text.set_label(text)

    def set_detail(self, detail: str) -> None:
        self.detail.set_label(detail)
        self.detail.set_visible(bool(detail))

    def set_text(self, text: str) -> None:
        self.text.set_label(text)

    def set_button(self, text: str, kind: str, sensitive: bool = True) -> None:
        self.button.set_label(text)
        for k in ("start", "stop"):
            self.button.remove_css_class(k)
        self.button.add_css_class(kind)
        self.button.set_sensitive(sensitive)


class LabeledBar(Gtk.Box):
    """'Overall progress            34 %' above a thick progress bar."""

    def __init__(self, title: str):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        row = hbox(8)
        self.title = label(title, "strong")
        self.title.set_hexpand(True)
        self.detail = label("", "caption", xalign=1.0)
        self.pct = label("0 %", "pct", xalign=1.0)
        row.append(self.title)
        row.append(self.detail)
        row.append(self.pct)
        self.bar = Gtk.ProgressBar()
        self.bar.add_css_class("big")
        self.append(row)
        self.append(self.bar)

    def set(self, pct: float, detail: str | None = None, tone: str | None = None) -> None:
        pct = max(0.0, min(100.0, pct))
        self.bar.set_fraction(pct / 100.0)
        self.pct.set_label(f"{pct:.0f} %")
        if detail is not None:
            self.detail.set_label(detail)
        for t in ("ok", "fail", "prep"):
            self.bar.remove_css_class(t)
        if tone:
            self.bar.add_css_class(tone)


class SequenceView(Gtk.Box):
    """The TM5-style row of test numbers for the current cycle, coloured by state."""

    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.add_css_class("card")
        head = hbox(10)
        self.title = label("Test sequence", "card-title")
        self.title.set_hexpand(True)
        head.append(self.title)
        for cls, text in (("pass", "passed"), ("fail", "errors"), ("running", "running"), ("", "waiting")):
            sw = Gtk.Box()
            sw.add_css_class("legend-swatch")
            if cls:
                sw.add_css_class(cls)
            sw.set_valign(Gtk.Align.CENTER)
            head.append(sw)
            head.append(label(text, "caption"))
        self.append(head)

        self.cycles = hbox(6)
        self.append(self.cycles)

        self.flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, homogeneous=True,
                                max_children_per_line=32, min_children_per_line=6,
                                row_spacing=6, column_spacing=6)
        self.flow.add_css_class("chips")
        self.append(self.flow)
        self.hint = label("Hover over a test number to see what it does.", "caption")
        self.append(self.hint)
        self.chips: list[Gtk.Label] = []
        self.cycle_tags: list[Gtk.Label] = []

    def set_sequence(self, seq: list[int], tests: dict[int, dict], cycles: int) -> None:
        child = self.flow.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.flow.remove(child)
            child = nxt
        self.chips = []
        for step, idx in enumerate(seq):
            chip = label(str(idx), "chip", xalign=0.5)
            t = tests.get(idx, {})
            tip = f"Step {step + 1} of {len(seq)} · Test #{idx} · {t.get('function', '?')}\n{t.get('summary', '')}"
            if t.get("explain"):
                tip += "\n\n" + t["explain"]
            chip.set_tooltip_text(tip)
            self.flow.insert(chip, -1)
            self.chips.append(chip)
        self.set_cycles(cycles)

    def set_cycles(self, cycles: int) -> None:
        child = self.cycles.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.cycles.remove(child)
            child = nxt
        self.cycle_tags = []
        shown = cycles if 0 < cycles <= 12 else 0
        for c in range(shown):
            tag = label(f"Cycle {c + 1}", "cycle-tag")
            self.cycles.append(tag)
            self.cycle_tags.append(tag)
        if not shown:
            self.cycles.append(label("Runs until you press Stop" if cycles == 0 else f"{cycles} cycles", "caption"))

    def mark_cycle(self, cycle: int, state: str) -> None:
        if 1 <= cycle <= len(self.cycle_tags):
            tag = self.cycle_tags[cycle - 1]
            for s in ("pass", "fail", "running"):
                tag.remove_css_class(s)
            if state:
                tag.add_css_class(state)
            tag.set_label(f"Cycle {cycle}" + {"pass": " ✔", "fail": " ✘", "running": " …"}.get(state, ""))

    def reset(self) -> None:
        for chip in self.chips:
            self.mark_chip(chip, "")

    def mark(self, step: int, state: str) -> None:
        if 0 <= step < len(self.chips):
            self.mark_chip(self.chips[step], state)

    @staticmethod
    def mark_chip(chip: Gtk.Label, state: str) -> None:
        for s in ("running", "pass", "fail"):
            chip.remove_css_class(s)
        if state:
            chip.add_css_class(state)


class ThreadsView(Gtk.Box):
    """Per-thread progress of the current test."""

    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.set_margin_top(14)
        self.set_margin_bottom(14)
        self.set_margin_start(14)
        self.set_margin_end(14)
        self.info = label("Every CPU thread tests its own slice of memory. Thread bars appear when a test starts.",
                          "caption", wrap=True)
        self.append(self.info)
        self.flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, homogeneous=True,
                                max_children_per_line=4, min_children_per_line=1, row_spacing=8, column_spacing=18)
        self.flow.add_css_class("threads")
        self.append(self.flow)
        self.rows: list[tuple[Gtk.ProgressBar, Gtk.Label]] = []

    def setup(self, threads: int, per_thread: str) -> None:
        child = self.flow.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            self.flow.remove(child)
            child = nxt
        self.rows = []
        self.info.set_label(f"{threads} threads, each testing its own {per_thread} slice of memory at the same time.")
        for i in range(threads):
            row = hbox(8)
            name = label(f"T{i:<3}", "mono", "dim")
            bar = Gtk.ProgressBar()
            bar.add_css_class("thin")
            bar.set_hexpand(True)
            bar.set_valign(Gtk.Align.CENTER)
            errs = label("", "bad", "strong")
            row.append(name)
            row.append(bar)
            row.append(errs)
            self.flow.insert(row, -1)
            self.rows.append((bar, errs))

    def complete(self) -> None:
        for bar, _errs in self.rows:
            bar.set_fraction(1.0)

    def update(self, pcts: list[float], errors: list[int]) -> None:
        for i, (bar, errs) in enumerate(self.rows):
            if i < len(pcts):
                bar.set_fraction(max(0.0, min(1.0, pcts[i] / 100.0)))
            e = errors[i] if i < len(errors) else 0
            errs.set_label(f"{e} err" if e else "")
            if e:
                bar.add_css_class("fail")


class LogView(Gtk.ScrolledWindow):
    """Timestamped activity log with coloured levels; mirrored to a log file."""

    def __init__(self):
        super().__init__()
        self.set_vexpand(True)
        self.view = Gtk.TextView(editable=False, cursor_visible=False, monospace=True,
                                 wrap_mode=Gtk.WrapMode.WORD_CHAR)
        self.view.add_css_class("log")
        for m in ("top", "bottom", "left", "right"):
            getattr(self.view, f"set_{m}_margin")(12)
        self.buf = self.view.get_buffer()
        self.buf.create_tag("time", foreground="#8591a6")
        self.buf.create_tag("info", foreground="#e9eef6")
        self.buf.create_tag("ok", foreground="#3fcf8e")
        self.buf.create_tag("warn", foreground="#f5b942")
        self.buf.create_tag("error", foreground="#ff6b6b", weight=Pango.Weight.BOLD)
        self.set_child(self.view)
        self.file = None
        self._scroll_pending = False

    ICON = {"info": "•", "ok": "✔", "warn": "▲", "error": "✘"}

    def add(self, level: str, text: str) -> None:
        level = level if level in self.ICON else "info"
        end = self.buf.get_end_iter()
        self.buf.insert_with_tags_by_name(end, f"{clock()}  ", "time")
        end = self.buf.get_end_iter()
        self.buf.insert_with_tags_by_name(end, f"{self.ICON[level]} {text}\n", level)
        if self.file:
            try:
                self.file.write(f"{clock()}  {level.upper():5} {text}\n")
                self.file.flush()
            except OSError:
                self.file = None
        if not self._scroll_pending:
            self._scroll_pending = True
            GLib.idle_add(self._scroll)

    def _scroll(self) -> bool:
        self._scroll_pending = False
        adj = self.get_vadjustment()
        adj.set_value(adj.get_upper() - adj.get_page_size())
        return False

    def clear(self) -> None:
        self.buf.set_text("")


class ErrorList(Gtk.ScrolledWindow):
    """Every memory error with the details needed to understand it."""

    MAX_ROWS = 1000

    def __init__(self):
        super().__init__()
        self.set_vexpand(True)
        self.list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE)
        self.list.add_css_class("errors")
        empty = vbox(6)
        empty.set_margin_top(30)
        empty.set_margin_bottom(30)
        empty.append(label("No errors", "good", "card-title", xalign=0.5))
        empty.append(label("Every value read back so far was exactly what was written.", "caption", xalign=0.5))
        self.list.set_placeholder(empty)
        self.set_child(self.list)
        self.count = 0

    def clear(self) -> None:
        child = self.list.get_first_child()
        while child:
            nxt = child.get_next_sibling()
            if isinstance(child, Gtk.ListBoxRow):
                self.list.remove(child)
            child = nxt
        self.count = 0

    def add(self, e: dict) -> None:
        self.count += 1
        if self.count > self.MAX_ROWS:
            return
        bits = int(e.get("bits", 0))
        persistent = e.get("kind") == "persistent"
        box = vbox(4)
        head = hbox(10)
        head.append(label(f"Error #{e.get('n', self.count)}", "bad", "strong"))
        head.append(label(f"Test #{e.get('test')} {e.get('function', '')} · cycle {e.get('cycle')} · "
                          f"step {int(e.get('step', 0)) + 1} · thread {e.get('thread')} · "
                          f"after {fmt_duration(e.get('elapsed'))}", "caption", wrap=True))
        box.append(head)
        box.append(label(f"Expected {e.get('expected')}    Read {e.get('actual')}    "
                         f"→ {bits} bit{'s' if bits != 1 else ''} wrong (bit {e.get('bit_list')})",
                         "mono", selectable=True, wrap=True))
        where = f"offset {e.get('offset')}"
        if e.get("paddr"):
            where += f" · physical address {e.get('paddr')}"
        kind = ("The wrong value is stored in RAM (reading it again gave the same bad value)." if persistent else
                "Read error: reading the same address again returned the correct value.")
        box.append(label(f"{kind}  ({where})", "caption", wrap=True, selectable=True))
        row = Gtk.ListBoxRow(activatable=False)
        row.set_child(box)
        self.list.append(row)
