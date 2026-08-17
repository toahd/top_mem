#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "blacklist.h"

// Knuth Hashing
// TODO: unhardcode mapsize or add table growth
#define KEY_SIZE (sizeof(uint32_t) * CHAR_BIT)
#define KNUTH_CONSTANT_32BIT 2654435769u

#define MASK (BL_MAP_SIZE - 1) // linear probing

#define MAX_TTL (UINT8_MAX / 2) + 1 // invariant req. for modular difference
#define BASE_CONSEC 1
#define MAX_CONSEC 7 // high values will exceed the half-range

/**
 * @brief Hashes a given PID.
 *
 * @details Using the Knuth multiplicative/fibinacci hashing, hashes the given
 * PID. This hashing technique was chosen for its ability to take a predictable
 * and linear input (PIDs from dirent are listed in ascending order) and still
 * uniformly populate a hashtable. Using this hash over something like integer
 * modulo or bitmasking benefits from a reduction in collision possibility.
 *
 * @param[in] pid PID to hash
 * @return Result of the hash, this gets used directly as an index
 */
static inline uint32_t hash(uint32_t pid) {
    static const uint32_t SHIFT_BY = (KEY_SIZE - BL_TABLE_BITS);
    return (pid * KNUTH_CONSTANT_32BIT) >> SHIFT_BY;
}

inline void tick_blacklist(blacklist *bl) { ++bl->ticks; }

static inline bool is_expired(uint8_t now, uint8_t then, uint8_t ttl) {
    return (uint8_t)(now - then) >= ttl;
}

static inline void grow_backoff(bl_entry *ent) {
    ent->ttl = (uint8_t)(BASE_TTL << ent->consec);
}

/**
 * @brief Checks if a PID is present in the Blacklist.
 *
 * @details Slightly overloaded function in terms of what operations are done,
 * side effects and the return values. The PID passed in gets hashed and then
 * that hash used as an index gets looked up in the blacklist. A pid of zero
 * means the slot in the table is empty. If the slot is not zero then the TTL
 * of that entry gets decremented before returning true. Otherwise, false is
 * returned and the idx pointer gets set to the idx of the entry.
 *
 * By passing back the hash that represents the would-be idx of the given PID
 * any overhead associated with a re-hash is avoided. However this does place
 * responsibility on the caller to pass that hash back to add_to_blacklist if
 * the PID is deemed to be blacklist-able.
 *
 * @param[in]     bl  Blacklist to search
 * @param[in]     pid PID to hash
 * @param[in,out] idx Resulting index that is produced from the hash
 *
 * @retval true  The PID already exists at the expected slot in the blacklist
 * @retval false PID was not found, expect idx pointer to be mutated with the
 * correct idx if this PID needs to get blacklisted
 */
bool is_blacklisted(blacklist *bl, uint32_t pid, uint32_t *idx) {
    *idx = hash(pid);

    // current slot has never been occupued
    if (bl->map[*idx].pid == 0)
        return false;

    // first open slot in a string of collisions is the idx to overwrite,
    // but the whole string must be walked
    uint32_t first_open_slot = UINT32_MAX;

    // slot is occupied
    for (uint32_t probe_count = 0; probe_count < BL_MAP_SIZE; probe_count++) {
        // Not a perfect indicator of persistance, but does identify
        // if a slot is a zombie
        bool live_last_scan = (uint8_t)(bl->ticks - bl->map[*idx].last_seen) <= 1u;

        // is the slot expired? is so is a candidate for reuse
        bool expired =
            is_expired(bl->ticks, bl->map[*idx].born_at, bl->map[*idx].ttl);

        // if the matching PID is in this slot then the probe ends
        if (bl->map[*idx].pid == pid) {
            // if the PID fails either eviction policy, then the
            // slot is a candidate for reuse
            if (expired || !live_last_scan)
                return false;

            // update last seen
            bl->map[*idx].last_seen = bl->ticks;

            // this slot is this PID and is still blacklisted!
            return true;
        }

        // The slot is occupied and is not this PID, check if it should
        // be evicted/the slot can be reused
        if (expired || !live_last_scan) {
            // Do not overwrite, make sure the earliest expiry gets
            // returned
            first_open_slot = (first_open_slot == UINT32_MAX ? *idx : first_open_slot);
        }

        // Slot was occupied, not assigned this PID and also not a
        // candidate for reuse. linear probe forward and check next
        // slot
        *idx = (*idx + 1) & MASK;

        // Did this string of collisions reach an unoccupied slot?
        if (bl->map[*idx].pid == 0) {
            if (first_open_slot != UINT32_MAX)
                *idx = first_open_slot;
            return false;
        }
    }

    // blacklist was full, reject this PID
    return false;
}

void blacklist_add(blacklist *bl, uint32_t pid, uint32_t idx) {
    // Was the previous resident of this slot the same PID?
    if (bl->map[idx].pid == pid) {
        // If so then refresh the expiry and grow exponential backoff
        // to reduce the amount of syscall attempts on this PID since
        // it is still failing
        if (bl->map[idx].consec < MAX_CONSEC)
            bl->map[idx].consec += 1;

        // refresh ticks, otherwise still comparing to old born_at time
        bl->map[idx].born_at = bl->ticks;

        bl->map[idx].last_seen = bl->ticks;

        grow_backoff(&bl->map[idx]);
    } else {
        // Otherwise this is an expired or brand new slot; full reset
        bl->map[idx] = (bl_entry){.pid = pid,
                                  .born_at = bl->ticks,
                                  .last_seen = bl->ticks,
                                  .ttl = BASE_TTL,
                                  .consec = BASE_CONSEC};

        ++bl->size;
    }
}
