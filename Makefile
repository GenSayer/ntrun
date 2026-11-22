# ntrun: make, make test TOOLS=/folder/with/the/tool/sets (see tests/run.sh), make clean
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
# MSYS2 UCRT64 (native Windows): a static program, so that a child ntrun does not need the
# DLLs of MSYS2 to be on the PATH that the program of the guest has
ifeq ($(OS),Windows_NT)
LDFLAGS += -static
endif
SRC = ntrun.c cmd.c seh.c cpu_mips.c cpu_alpha.c cpu_ppc.c cpu_x86.c

ntrun: $(SRC) ntrun.h x86args.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) -lm -lpthread

test: ntrun
	sh tests/run.sh $(TOOLS)

clean:
	rm -f ntrun ntrun.exe

.PHONY: test clean
