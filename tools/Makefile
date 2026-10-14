# Host tools. Build with: make -C tools
CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
CORE = ../src/core
BIN = ../build/host

TOOLS = $(BIN)/umdinfo

all: $(TOOLS)

$(BIN)/umdinfo: umdinfo.c host_source.c $(CORE)/iso9660.c $(CORE)/sfo.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
