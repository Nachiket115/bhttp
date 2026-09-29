CC = gcc
CFLAGS = -Wall -Wextra -O2

all: bin/bserve bin/bcurl

bin/bserve: src/bserve.c src/bhttp.h | bin
	$(CC) $(CFLAGS) -o $@ src/bserve.c

bin/bcurl: src/bcurl.c src/bhttp.h | bin
	$(CC) $(CFLAGS) -o $@ src/bcurl.c

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
