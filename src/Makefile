CC = gcc
CFLAGS = -Wall -Wextra
SRC = 2_1_rdma_init_test.c
BIN = rdma_init_test
LDFLAGS = -libverbs

.PHONY: all build run clean

all: build

build:
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

run: build
	./$(BIN)
	rm -f $(BIN)

clean:
	rm -f $(BIN)
