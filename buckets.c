#include "buckets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUCKETS 512

typedef struct {
    char name[NAME_LEN];
    long summed_vmrss;
} bucket;

static bucket buckets[MAX_BUCKETS];
static size_t bucket_count = 0;

void add_bucket(const char *name, long vmrss) {
    // guard clause against failed parses
    if (!name[0] || vmrss <= 0)
        return;

    // check if an existing bucket holds the same name
    for (size_t i = 0; i < bucket_count; i++) {
        if (strcmp(buckets[i].name, name) == 0) {
            buckets[i].summed_vmrss += vmrss;
            return;
        }
    }

    // otherwise, add as a new bucket
    if (bucket_count < MAX_BUCKETS) {
        snprintf(buckets[bucket_count].name, sizeof buckets[bucket_count].name,
                 "%s", name);
        buckets[bucket_count].summed_vmrss = vmrss;
        bucket_count++;
    }
}

static int sort_vmrss_desc(const void *a, const void *b) {
    long va = ((const bucket *)a)->summed_vmrss,
         vb = ((const bucket *)b)->summed_vmrss;
    return (vb > va) - (vb < va);
}

void clear_buckets() {
    memset(buckets, 0, bucket_count * sizeof(bucket));
    bucket_count = 0;
}

void print_buckets(int count) {
    qsort(buckets, bucket_count, sizeof buckets[0], sort_vmrss_desc);

    printf("PRINTING BUCKETS: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("%s %ld kB\n", buckets[i].name, buckets[i].summed_vmrss);
    }
}
