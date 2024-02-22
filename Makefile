SRC=$(wildcard src/*.c)
SRC+=test.c
BIN?=palloc-test
CC?=gcc

override CFLAGS?=-Wall -s -O2

include lib/.dep/config.mk

$(BIN): $(SRC) src/palloc.h
	$(CC) -Isrc $(INCLUDES) $(CFLAGS) -o $@ $(SRC)

.PHONY: check
check: $(BIN)
	./$<

.PHONY: clean
clean:
	rm -f $(BIN)
