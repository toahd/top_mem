CC       := gcc
CFLAGS   := -Wall -Wextra -Werror -Wconversion -std=gnu11 -O3 -flto -static -march=native
CPPFLAGS := -I. -Iinclude 
LDFLAGS  := -flto

TARGET   := top_mem
OBJS     := top_mem.o buckets.o blacklist.o whitelist.o
DEPS     := $(OBJS=.o=.d)

VPATH    := src

FIND     := find
RM       := rm -f

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

.PHONY: format-all
format-all:
	@command -v clang-format >/dev/null || { echo "[ERROR] clang-format not installed"; exit 1; }
	$(FIND) . \( -name "*.c" -o -name "*.h" \) -exec clang-format -i --style=file {} +

format-%:
	clang-format -i --style=file $*

.PHONY: clean
clean:
	$(RM) $(TARGET) *.o

