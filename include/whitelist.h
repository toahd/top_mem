#ifndef WHITELIST_H // WHITELIST_H
#define WHITELIST_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "configs.h"

#define WL_TABLE_BITS 12 // 2^11 = 2048, used to get table/map size
#define WL_MAP_SIZE (1u << WL_TABLE_BITS)

typedef struct {
    uint32_t pid; // required to keep in case of collisions
    uint8_t born_at; // tick count this entry was first recorded
    uint8_t last_seen; // tick count this entry was last seen, useful
    // to know if an entry can be evicted before ttl
    uint8_t ttl; // tick counts until a entry must be retried
    uint8_t consec; // consecuative times entry has been re-added
    // used to grow exponential backoff (ttl)
    char comm_name[COMM_LEN]; // cached comm name
} wl_entry;

typedef struct {
    wl_entry map[WL_MAP_SIZE];
    uint8_t ticks;
    size_t size;
} whitelist;

void tick_whitelist(whitelist *wl);
void whitelist_add(whitelist *wl, uint32_t pid, const char *name, uint32_t idx);
bool is_whitelisted(whitelist *wl, uint32_t pid, uint32_t *idx);

#endif // WHITELIST_H
