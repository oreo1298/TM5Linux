Name:           tm5linux
Version:        1.0.0
Release:        1%{?dist}
Summary:        RAM stability tester that runs TestMem5 (TM5) configs
License:        MIT
URL:            https://github.com/oreo1298/TM5Linux
Source0:        tm5linux-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
Requires:       python3
%if 0%{?suse_version}
Requires:       python3-gobject
Requires:       python3-gobject-Gdk
Requires:       typelib-1_0-Gtk-4_0
Requires:       libcap-progs
%else
Requires:       python3-gobject
Requires:       gtk4
Requires:       libcap
%endif
Recommends:     polkit

%description
TM5 for Linux tests memory stability like TestMem5 on Windows. It reads TM5
config files directly and ships the Extreme1 profile by anta777, ABSOLUT,
Universal2 and PCBDestroyer. Comes with a dark GTK 4 interface (tm5linux) and
a terminal version (tm5linux-cli).

%prep
%autosetup -n tm5linux-%{version}

%build
%make_build PREFIX=%{_prefix}

%check
./build/tm5linux-cli --selftest

%install
%make_install PREFIX=%{_prefix}
rm -f %{buildroot}%{_datadir}/tm5linux/uninstall.sh

%files
%license LICENSE
%caps(cap_ipc_lock=ep) %{_bindir}/tm5linux-cli
%{_bindir}/tm5linux
%{_datadir}/tm5linux/
%{_datadir}/doc/tm5linux/
%{_datadir}/applications/io.github.oreo1298.TM5Linux.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.oreo1298.TM5Linux.svg
%{_datadir}/metainfo/io.github.oreo1298.TM5Linux.metainfo.xml
%{_mandir}/man1/tm5linux-cli.1*

%changelog
* Thu Oct 08 2026 TM5 for Linux contributors <tm5linux@users.noreply.github.com> - 1.0.0-1
- First release
