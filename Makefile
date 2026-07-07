CC      ?= cc
CFLAGS  += -O2 -std=c11 -Wall -Wextra
WL_CFLAGS := $(shell pkg-config --cflags wayland-client)
WL_LIBS   := $(shell pkg-config --libs wayland-client)
PROTO_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_XML   := $(PROTO_DIR)/stable/xdg-shell/xdg-shell.xml

OBJ := build/main.o build/game.o build/draw.o build/sound.o \
       build/xdg-shell-protocol.o

crateblast: $(OBJ)
	$(CC) -o $@ $(OBJ) $(WL_LIBS) -lm -ldl -pthread

build/xdg-shell-client-protocol.h: $(XDG_XML) | build
	wayland-scanner client-header $< $@

build/xdg-shell-protocol.c: $(XDG_XML) | build
	wayland-scanner private-code $< $@

build/xdg-shell-protocol.o: build/xdg-shell-protocol.c
	$(CC) $(CFLAGS) $(WL_CFLAGS) -c -o $@ $<

build/%.o: src/%.c src/game.h build/xdg-shell-client-protocol.h | build
	$(CC) $(CFLAGS) $(WL_CFLAGS) -Ibuild -c -o $@ $<

build:
	mkdir -p build

run: crateblast
	./crateblast map.txt

edit:
	python3 editor.py map.txt

clean:
	rm -rf build crateblast

.PHONY: run edit clean
