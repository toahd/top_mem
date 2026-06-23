CC := gcc
CFLAGS := -Wall -Wextra -Werror -std=gnu11 -O3 -static -march=native

TARGET := top_mem
SRC := $(TARGET).c

.PHONY: all
all: $(TARGET)

profile:
	gcc -Wall -Wextra -Werror -std=gnu11 -O3 -g -static -fno-omit-frame-pointer -march=native -DPROFILE ./top_mem.c -o top_mem

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: clean
clean:
	rm -f $(TARGET)

