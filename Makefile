# ntrun: make, make test TOOLS=/folder/with/the/tool/sets (see tests/run.sh), make clean
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation
SRC = ntrun.c cmd.c seh.c cpu_mips.c cpu_alpha.c cpu_ppc.c cpu_x86.c

ntrun: $(SRC) ntrun.h x86args.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) -lm -lpthread

test: ntrun
	sh tests/run.sh $(TOOLS)

clean:
	rm -f ntrun

.PHONY: test clean
