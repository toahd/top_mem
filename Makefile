CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=gnu11 -O3 -static -march=native

TARGET := top_mem
SRC := $(TARGET).c

.PHONY: all
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f $(TARGET)

