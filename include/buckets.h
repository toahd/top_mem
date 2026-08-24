#ifndef BUCKETS_H
#define BUCKETS_H
#include <stddef.h>

#define NAME_LEN 16
void add_bucket(const char *name, long vmrss);
void clear_buckets();
void print_buckets(int count);
#endif
