CFLAGS ?= -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE
PREFIX ?= $(HOME)/.local

all: build/p1505-printer-app build/test_xqx

build/p1505-printer-app: src/main.c src/xqx.c src/xqx.h | build
	$(CC) $(CFLAGS) $(shell pkg-config --cflags pappl) -o $@ src/main.c src/xqx.c $(shell pkg-config --libs pappl) -ljbig

build/test_xqx: tests/test_xqx.c src/xqx.c src/xqx.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_xqx.c src/xqx.c -ljbig

build:
	mkdir -p build

# Round trip against the HPLIP reference stream in ref/: pixels and JBIG bytes must match.
test: build/test_xqx
	build/test_xqx unit
	python3 -I tests/xqxdump.py ref/hplip-Normal.bin --jbig build/hp >/dev/null
	build/test_xqx encode build/hp-1.jbg build/ours.bin
	python3 -I tests/xqxdump.py build/ours.bin --jbig build/ours >/dev/null
	build/test_xqx cmp build/hp-1.jbg build/ours-1.jbg
	cmp build/hp-1.jbg build/ours-1.jbg && echo "JBIG byte-identical to HPLIP"

install: build/p1505-printer-app
	install -Dm755 build/p1505-printer-app $(PREFIX)/bin/p1505-printer-app

clean:
	rm -rf build

.PHONY: all test install clean
