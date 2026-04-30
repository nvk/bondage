CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -std=c99 -pedantic
CPPFLAGS ?= -Iinclude
LDFLAGS ?=

BIN := bondage
OBJ := src/main.o src/config.o src/verify.o src/launch.o src/repin.o

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c include/%.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(BIN) src/*.o
