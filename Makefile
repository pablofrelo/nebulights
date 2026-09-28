PREFIX          ?= /usr/local
VERSION ?= 0.1.0
WAYLAND_SCANNER ?= wayland-scanner

CFLAGS  ?= -O2 -Wall -Wextra -pedantic
CFLAGS  += -std=c11 -Isrc -DVERSION='"$(VERSION)"'
CFLAGS  += $(shell pkg-config --cflags wayland-client wayland-egl egl glesv2)
LDLIBS  := $(shell pkg-config --libs wayland-client wayland-egl egl glesv2) -lm

PROTOCOLS = xdg-shell wlr-layer-shell-unstable-v1 ext-idle-notify-v1
PROTO_H   = $(PROTOCOLS:%=src/%-client-protocol.h)
PROTO_C   = $(PROTOCOLS:%=src/%-client-protocol.c)

SRC = src/main.c src/scene.c src/inhibit.c $(PROTO_C)
OBJ = $(SRC:.c=.o)

# D-Bus idle inhibitor detection needs sd-bus; without libsystemd
# the build still works, just without that feature.
ifeq ($(shell pkg-config --exists libsystemd && echo yes),yes)
CFLAGS  += -DHAVE_SDBUS $(shell pkg-config --cflags libsystemd)
LDLIBS  += $(shell pkg-config --libs libsystemd)
endif

all: nebulights

src/%-client-protocol.h: protocols/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

src/%-client-protocol.c: protocols/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

$(OBJ): $(PROTO_H)

nebulights: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

install: nebulights
	install -Dm755 nebulights $(DESTDIR)$(PREFIX)/bin/nebulights
	install -Dm644 contrib/nebulights.service $(DESTDIR)$(PREFIX)/lib/systemd/user/nebulights.service
	install -Dm644 nebulights.conf.example $(DESTDIR)$(PREFIX)/share/doc/nebulights/nebulights.conf.example
	install -Dm644 contrib/nebulights.desktop $(DESTDIR)$(PREFIX)/share/doc/nebulights/nebulights.desktop
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/nebulights/README.md
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/nebulights/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nebulights \
	      $(DESTDIR)$(PREFIX)/lib/systemd/user/nebulights.service
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/nebulights \
	       $(DESTDIR)$(PREFIX)/share/licenses/nebulights

clean:
	rm -f nebulights $(OBJ) $(PROTO_H) $(PROTO_C) test/fast

.PHONY: all install uninstall clean
