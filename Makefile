TARGET = spinlock_test
BIN_DIR = bin
SRCS = spinlock_test.c test.c
CC = gcc
CFLAGS = -O3 -Wall -Wextra -std=gnu99
LDLIBS = -pthread -lrt

.PHONY: all clean

all: $(BIN_DIR)/$(TARGET)

$(BIN_DIR)/$(TARGET): $(SRCS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDLIBS)

clean:
	rm -rf $(BIN_DIR) *.o
