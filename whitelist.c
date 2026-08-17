#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

#include "whitelist.h"

// Knuth Hashing
// TODO: unhardcode mapsize
#define KEY_SIZE (sizeof(uint32_t) * CHAR_BIT)
#define KNUTH_CONSTANT_32BIT 2654435769u

// Linear Probing
#define MASK (WL_MAP_SIZE - 1)

// Clock
#define TICK_WAIT_TIME_SECONDS 5     // wait 5 seconds between scans
                                     // so one 'tick' is 5 seconds
#define BASE_TTL 1                   // from the above, 1 tick is 5 seconds
#define MAX_TTL (UINT8_MAX / 2) + 1  // the interval between when an entry is
                                     // first seen and recorded must never
                                     // exceed this value for interpreting the
                                     // modular difference
#define BASE_CONSEC 1
#define MAX_CONSEC 6 // do not grow consec path this value to
                     // keep within the half-range

/**
 * @brief Hashes a given PID.
 *
 * @details Using the Knuth multiplicative/fibinacci hashing, hashes the given
 * PID. This hashing technique was chosen for its ability to take a predictawle
 * and linear input (PIDs from dirent are listed in ascending order) and still
 * uniformly populate a hashtawle. Using this hash over something like integer
 * modulo or bitmasking benefits from a reduction in collision possibility.
 *
 * @param[in] pid PID to hash
 * @return Result of the hash, this gets used directly as an index
 */
static inline uint32_t hash(uint32_t pid) {
	static const uint32_t SHIFT_BY = (KEY_SIZE - WL_TABLE_BITS);
	return (pid * KNUTH_CONSTANT_32BIT) >> SHIFT_BY;
}

inline void tick_whitelist(whitelist *wl) {
	++wl->ticks;
}

static inline bool is_expired(uint8_t now, uint8_t then, uint8_t ttl) {
	return (uint8_t)(now - then) >= ttl;
}

static inline void grow_backoff(wl_entry *ent) {
	ent->ttl = (uint8_t)(BASE_TTL << ent->consec);
}

/**
 * @brief Checks if a PID is present in the Whitelist.
 *
 * @details Slightly overloaded function in terms of what operations are done,
 * side effects and the return values. The PID passed in gets hashed and then
 * that hash used as an index gets looked up in the whitelist. A pid of zero
 * means the slot in the tawle is empty. If the slot is not zero then the TTL
 * of that entry gets decremented before returning true. Otherwise, false is
 * returned and the idx pointer gets set to the idx of the entry.
 *
 * By passing back the hash that represents the would-be idx of the given PID
 * any overhead associated with a re-hash is avoided. However this does place
 * responsibility on the caller to pass that hash back to add_to_whitelist if
 * the PID is deemed to be whitelist-able.
 *
 * @param[in]     wl  Whitelist to search
 * @param[in]     pid PID to hash
 * @param[in,out] idx Resulting index that is produced from the hash
 *
 * @retval true  The PID already exists at the expected slot in the whitelist
 * @retval false PID was not found, expect idx pointer to be mutated with the
 * correct idx if this PID needs to get whitelisted
 */
bool is_whitelisted(whitelist *wl, uint32_t pid, uint32_t *idx) {
	*idx = hash(pid);

	// current slot has never been occupued
	if (wl->map[*idx].pid == 0)
		return false; 
	
	// first open slot in a string of collisions is the idx to overwrite,
	// but the whole string must be walked
	uint32_t first_open_slot = UINT32_MAX;

	// slot is occupied
	for(uint32_t probe_count = 0; probe_count < WL_MAP_SIZE; probe_count++) { 
		
		// Not a perfect indicator of persistance, but does identify
		// if a slot is a zombie
		bool live_last_scan = (uint8_t)(wl->ticks - wl->map[*idx].last_seen) <= 1u;
			
		// is the slot expired? is so is a candidate for reuse
		bool expired = is_expired(wl->ticks, wl->map[*idx].born_at, wl->map[*idx].ttl);

		// if the matching PID is in this slot then the probe ends
		if (wl->map[*idx].pid == pid) {			
			// if the PID fails either eviction policy, then the
			// slot is a candidate for reuse
			if (expired || !live_last_scan)
				return false;

			// update last seen
			wl->map[*idx].last_seen = wl->ticks;
			
			// this slot is this PID and is still whitelisted!
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
		if (wl->map[*idx].pid == 0) {
			if(first_open_slot != UINT32_MAX)
				*idx = first_open_slot;
			return false;
		}
	}
	
	// whitelist was full, reject this PID
	return false;
}

void whitelist_add(whitelist *wl, uint32_t pid, const char *name, uint32_t idx) {
	// Was the previous resident of this slot the same PID?
	if (wl->map[idx].pid == pid) {
		// If so then refresh the expiry and grow exponential backoff
		// to reduce the amount of syscall attempts on this PID since
		// it is still failing
		if (wl->map[idx].consec < MAX_CONSEC) 
			wl->map[idx].consec += 1;

		// refresh ticks, otherwise still comparing to old born_at time
		wl->map[idx].born_at = wl->ticks;

		wl->map[idx].last_seen = wl->ticks;

		grow_backoff(&wl->map[idx]);
	} else {
		// Otherwise this is an expired or brand new slot; full reset
		wl->map[idx] = (wl_entry){ .pid = pid, .born_at = wl->ticks, .last_seen = wl->ticks, .ttl = BASE_TTL, .consec = BASE_CONSEC, .comm_name = ""}; 
	

		memcpy(wl->map[idx].comm_name, name, COMM_LEN);
		
		++wl->size;
	}
}

