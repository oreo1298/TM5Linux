# TM5 for Linux - build & install
#
#   make                      build the engine (build/tm5linux-cli)
#   make check                run the engine self-test and GUI syntax checks
#   sudo make install         install to /usr/local (PREFIX=/usr for distro packages)
#   sudo make uninstall       remove what "make install" installed
#
# Packagers: use DESTDIR=... and PREFIX=/usr. "make install" does not grant the memory-lock
# capability; run "make setcap" (as root) or do it in the package's post-install hook.

VERSION   := 1.0.0
APP_ID    := io.github.oreo1298.TM5Linux

PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DATADIR   ?= $(PREFIX)/share
PKGDATADIR = $(DATADIR)/tm5linux
DOCDIR    ?= $(DATADIR)/doc/tm5linux
DESTDIR   ?=

CC        ?= cc
CFLAGS    ?= -O2 -g
CFLAGS    += -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter -pthread
CPPFLAGS  += -D_GNU_SOURCE -DTM5L_VERSION=\"$(VERSION)\" -DTM5L_DATADIR=\"$(PKGDATADIR)\"
LDFLAGS   +=
LDLIBS    += -pthread -lm

INSTALL   ?= install
PYTHON    ?= python3

SRC := $(wildcard engine/*.c)
OBJ := $(patsubst engine/%.c,build/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
BIN := build/tm5linux-cli

.PHONY: all check install uninstall setcap clean dist FORCE

all: $(BIN) build/tm5linux

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: engine/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	@mkdir -p build

# The install paths are compiled in, so rebuild what uses them when PREFIX changes.
build/.paths: FORCE | build
	@echo '$(PKGDATADIR) $(BINDIR)' | cmp -s - $@ 2>/dev/null || echo '$(PKGDATADIR) $(BINDIR)' > $@

build/config.o: build/.paths

FORCE:

# The GUI launcher with the install location baked in.
build/tm5linux: gui/tm5linux.in build/.paths
	sed -e 's|@PKGDATADIR@|$(PKGDATADIR)|g' -e 's|@BINDIR@|$(BINDIR)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@
	chmod 755 $@

check: $(BIN)
	./$(BIN) --selftest
	$(PYTHON) -m py_compile gui/tm5linux/*.py
	@echo "All checks passed."

install: all
	$(INSTALL) -Dm755 $(BIN)                     $(DESTDIR)$(BINDIR)/tm5linux-cli
	$(INSTALL) -Dm755 build/tm5linux             $(DESTDIR)$(BINDIR)/tm5linux
	$(INSTALL) -d                                $(DESTDIR)$(PKGDATADIR)/gui/tm5linux
	$(INSTALL) -m644 gui/tm5linux/*.py gui/tm5linux/*.css $(DESTDIR)$(PKGDATADIR)/gui/tm5linux/
	$(INSTALL) -d                                $(DESTDIR)$(PKGDATADIR)/configs
	$(INSTALL) -m644 configs/*.cfg               $(DESTDIR)$(PKGDATADIR)/configs/
	$(INSTALL) -Dm755 uninstall.sh               $(DESTDIR)$(PKGDATADIR)/uninstall.sh
	$(INSTALL) -Dm644 data/$(APP_ID).desktop     $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
	$(INSTALL) -Dm644 data/$(APP_ID).svg         $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg
	$(INSTALL) -Dm644 data/$(APP_ID).metainfo.xml $(DESTDIR)$(DATADIR)/metainfo/$(APP_ID).metainfo.xml
	$(INSTALL) -Dm644 data/tm5linux-cli.1        $(DESTDIR)$(DATADIR)/man/man1/tm5linux-cli.1
	$(INSTALL) -Dm644 README.md                  $(DESTDIR)$(DOCDIR)/README.md
	$(INSTALL) -Dm644 docs/TESTS.md              $(DESTDIR)$(DOCDIR)/TESTS.md
	$(INSTALL) -Dm644 LICENSE                    $(DESTDIR)$(DOCDIR)/LICENSE

uninstall:
	rm -f  $(DESTDIR)$(BINDIR)/tm5linux-cli $(DESTDIR)$(BINDIR)/tm5linux
	rm -rf $(DESTDIR)$(PKGDATADIR) $(DESTDIR)$(DOCDIR)
	rm -f  $(DESTDIR)$(DATADIR)/applications/$(APP_ID).desktop
	rm -f  $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/$(APP_ID).svg
	rm -f  $(DESTDIR)$(DATADIR)/metainfo/$(APP_ID).metainfo.xml
	rm -f  $(DESTDIR)$(DATADIR)/man/man1/tm5linux-cli.1

# Let normal users lock memory (no root needed while testing).
setcap:
	setcap cap_ipc_lock=ep $(DESTDIR)$(BINDIR)/tm5linux-cli

clean:
	rm -rf build

dist:
	git archive --format=tar.gz --prefix=tm5linux-$(VERSION)/ -o tm5linux-$(VERSION).tar.gz HEAD

-include $(DEP)
