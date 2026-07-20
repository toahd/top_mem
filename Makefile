CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -std=gnu11 -O3 -flto -static -march=native
LDFLAGS := -flto

OBJS    := top_mem.o buckets.o
TARGET  := top_mem
SRC     := $(TARGET).c

RM      := rm -f

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

top_mem.o buckets.o: buckets.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	$(RM) $(TARGET) $(OBJS)

