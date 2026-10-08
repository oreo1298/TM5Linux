"""Application entry point."""
from __future__ import annotations

import os
import sys

try:
    import gi

    gi.require_version("Gtk", "4.0")
    from gi.repository import Gdk, Gio, GLib, Gtk
except (ImportError, ValueError) as exc:  # pragma: no cover - shown to the user
    sys.stderr.write(
        "TM5 for Linux needs GTK 4 and PyGObject.\n"
        "  Arch:           sudo pacman -S python-gobject gtk4\n"
        "  Debian/Ubuntu:  sudo apt install python3-gi gir1.2-gtk-4.0\n"
        "  Fedora:         sudo dnf install python3-gobject gtk4\n"
        "  openSUSE:       sudo zypper install python3-gobject typelib-1_0-Gtk-4_0\n"
        f"({exc})\n"
        "The command line version works without them: tm5linux-cli --help\n")
    sys.exit(1)

from . import APP_ID, APP_NAME, VERSION, WEBSITE
from .window import MainWindow


class App(Gtk.Application):
    def __init__(self, engine_hint: str | None = None):
        flags = getattr(Gio.ApplicationFlags, "DEFAULT_FLAGS", Gio.ApplicationFlags.FLAGS_NONE)
        super().__init__(application_id=APP_ID, flags=flags)
        self.engine_hint = engine_hint
        GLib.set_application_name(APP_NAME)

    def do_startup(self) -> None:
        Gtk.Application.do_startup(self)
        settings = Gtk.Settings.get_default()
        if settings is not None:
            settings.set_property("gtk-application-prefer-dark-theme", True)
        css = Gtk.CssProvider()
        css.load_from_path(os.path.join(os.path.dirname(os.path.abspath(__file__)), "style.css"))
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css,
                                                  Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        Gtk.Window.set_default_icon_name(APP_ID)

        for name, cb, accels in (("about", self.on_about, []), ("quit", self.on_quit, ["<Control>q"])):
            act = Gio.SimpleAction.new(name, None)
            act.connect("activate", cb)
            self.add_action(act)
            if accels:
                self.set_accels_for_action(f"app.{name}", accels)
        self.set_accels_for_action("win.load-config", ["<Control>o"])

    def do_activate(self) -> None:
        win = self.props.active_window
        if not win:
            win = MainWindow(self, self.engine_hint)
        win.present()

    def on_quit(self, *_args) -> None:
        win = self.props.active_window
        if win:
            win.close()
        else:
            self.quit()

    def on_about(self, *_args) -> None:
        about = Gtk.AboutDialog(transient_for=self.props.active_window, modal=True)
        about.set_program_name(APP_NAME)
        about.set_version(VERSION)
        about.set_logo_icon_name(APP_ID)
        about.set_comments("A RAM stability tester for Linux that runs TestMem5 (TM5) configs such as "
                           "Extreme1 by anta777.")
        about.set_website(WEBSITE)
        about.set_website_label("Project page")
        about.set_license_type(Gtk.License.MIT_X11)
        about.set_authors(["TM5 for Linux contributors"])
        about.add_credit_section("Test profiles", ["anta777 (Extreme1, ABSOLUT)", "LMHz (Universal2)",
                                                   "Mechan0 (PCBDestroyer)", "Collected by integralfx/MemTestHelper"])
        about.add_credit_section("Inspired by", ["TestMem5 by Serj (testmem.tz.ru)"])
        about.present()


def main(engine_hint: str | None = None, version: str | None = None) -> int:
    app = App(engine_hint)
    return app.run(sys.argv[:1])
