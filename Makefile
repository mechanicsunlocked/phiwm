# phiwm build
#   Linux (cross):  make              needs mingw-w64
#   Windows MSYS2:  make CROSS=       in a UCRT64/MINGW64 shell
#   Tests (native): make test

VERSION := 0.2.0
CROSS   ?= x86_64-w64-mingw32-
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres
HOSTCC  ?= cc

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -municode -DUNICODE -D_UNICODE
LDFLAGS := -static -s -mwindows -municode
LIBS    := -luser32 -lgdi32 -lshell32 -ldwmapi -ladvapi32 -lshcore -lole32

SRC := src/wm.c src/keys.c src/tray.c src/util.c src/defaults.c src/layout.c
OBJ := $(patsubst src/%.c,build/%.o,$(SRC)) build/phiwm.res.o

all: build/phiwm.exe

build:
	mkdir -p build

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/phiwm.res.o: res/phiwm.rc res/phiwm.ico res/phiwm.manifest | build
	$(WINDRES) -I res res/phiwm.rc -O coff -o $@

build/phiwm.exe: $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

test: | build
	$(HOSTCC) -std=c99 -Wall -Wextra -O1 -g -Isrc tests/test_layout.c src/layout.c -o build/test_layout
	$(HOSTCC) -std=c99 -Wall -Wextra -O1 -g -Isrc tests/test_defaults.c src/defaults.c -o build/test_defaults
	./build/test_layout
	./build/test_defaults

dist: build/phiwm.exe
	rm -f build/phiwm-$(VERSION)-x64.zip
	cd build && cp ../README.md ../LICENSE . && zip -9 phiwm-$(VERSION)-x64.zip phiwm.exe README.md LICENSE

clean:
	rm -rf build

.PHONY: all test dist clean
