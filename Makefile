SRC=$(wildcard src/*.c)
SRC+=test.c
BIN?=palloc-test
CC?=gcc

TAIL=$(shell command -v gtail tail | head -1)
HEAD=$(shell command -v ghead head | head -1)

override CFLAGS?=-Wall -s -O2

include lib/.dep/config.mk

.PHONY: default
default: README.md ${BIN}

${BIN}: ${SRC} src/palloc.h
	${CC} -Isrc ${INCLUDES} ${CFLAGS} -o $@ ${SRC}

.PHONY: check
check: ${BIN}
	./$<

.PHONY: clean
clean:
	rm -f ${BIN}

README.md: ${SRC} src/palloc.h
	stddoc < src/palloc.h > README.md
