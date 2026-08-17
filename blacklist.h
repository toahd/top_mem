#ifndef BLACKLIST_H // BLACKLIST_H
#define BLACKLIST_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define BL_TABLE_BITS 11 // 2^11 = 2048, used to get table/map size
#define BL_MAP_SIZE (1u << BL_TABLE_BITS)

typedef struct {
    uint32_t pid;      // required to keep in case of collisions
    uint8_t born_at;   // tick count this entry was first recorded
    uint8_t last_seen; // tick count this entry was last seen, useful
                       // to know if an entry can be evicted before ttl
    uint8_t ttl;       // tick counts until a entry must be retried
    uint8_t consec;    // consecuative times entry has been re-added
                       // used to grow exponential backoff (ttl)
} bl_entry;

typedef struct {
    bl_entry map[BL_MAP_SIZE];
    uint8_t ticks;
    size_t size;
} blacklist;

void tick_blacklist(blacklist *bl);
void blacklist_add(blacklist *bl, uint32_t pid, uint32_t idx);
bool is_blacklisted(blacklist *bl, uint32_t pid, uint32_t *idx);

#endif // BLACKLIST_H
