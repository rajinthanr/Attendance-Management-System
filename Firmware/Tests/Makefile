# Host build of the Level 2 logic. If this ever needs an STM32 header, the
# layering has been broken.
CC      ?= cc
CFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow \
           -Wconversion -Wsign-conversion -Wundef \
           -I../App/Inc
# Every Level 2 source, app_fsm.c included. The FSM is not driven by these
# tests -- it needs a real platform to be worth running -- but compiling it
# here still holds it to the host's stricter warning set.
SRC     := $(wildcard ../App/Src/*.c) host_platform.c test_main.c
BIN     := run_tests

all: $(BIN)
	./$(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(BIN)

.PHONY: all clean
