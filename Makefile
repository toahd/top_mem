CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -Wconversion -std=gnu11 -O3 -flto -static -march=native
LDFLAGS := -flto

OBJS    := top_mem.o buckets.o blacklist.o whitelist.o read_exe.o parse_pid.o read_comm.o parse_statm.o
TARGET  := top_mem

FIND    := find
RM      := rm -f

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

top_mem.o whitelist.o parse_pid.o read_comm.o parse_statm.o read_exe.o: configs.h
top_mem.o buckets.o: buckets.h
top_mem.o blacklist.o: blacklist.h
top_mem.o whitelist.o: whitelist.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: format-all
format-all:
	@command -v clang-format >/dev/null || { echo "[ERROR] clang-format not installed"; exit 1; }
	$(FIND) . \( -name "*.c" -o -name "*.h" \) -exec clang-format -i --style=file {} +

format-%:
	clang-format -i --style=file $*

.PHONY: clean
clean:
	$(RM) $(TARGET) $(OBJS)

