CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= $(HOME)/.local
AUTOSTART ?= $(HOME)/.config/autostart

trueedged: src/trueedged.c
	$(CC) $(CFLAGS) -o $@ $< -lX11 -lXi -lXrandr -lXfixes -lm

install: trueedged
	install -Dm755 trueedged $(PREFIX)/bin/trueedged
	install -Dm755 trueedge-config $(PREFIX)/bin/trueedge-config
	install -Dm644 trueedged.desktop $(AUTOSTART)/trueedged.desktop

uninstall:
	rm -f $(PREFIX)/bin/trueedged $(PREFIX)/bin/trueedge-config $(AUTOSTART)/trueedged.desktop

clean:
	rm -f trueedged

.PHONY: install uninstall clean
