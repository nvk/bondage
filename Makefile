CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BIN := bondage
UNAME := $(shell uname)
SWIFTC ?= swiftc
EXTRAS :=
ifeq ($(UNAME), Darwin)
EXTRAS += touchid-check
endif

OBJ := src/main.o src/config.o src/verify.o src/launch.o src/repin.o

.PHONY: all clean test

all: $(BIN) $(EXTRAS)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

touchid-check: touchid-check.swift
	$(SWIFTC) -O -o $@ touchid-check.swift

src/%.o: src/%.c include/config.h include/launch.h include/main.h include/repin.h include/verify.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: all
	./tests/test-config-defaults.sh
	./tests/test-cli-options.sh

clean:
	rm -f $(BIN) touchid-check src/*.o
