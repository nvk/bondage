CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BIN := bondage
OBJ := src/main.o src/config.o src/verify.o src/launch.o src/repin.o

.PHONY: all clean test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c include/config.h include/launch.h include/main.h include/repin.h include/verify.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: all
	./tests/test-config-defaults.sh

clean:
	rm -f $(BIN) src/*.o
