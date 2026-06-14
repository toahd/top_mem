#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

/*
 *  __________
 * /__   ____/      ___       ___
 *    | | ___   ____| |     __| |
 *    | |/ _ \ / _  | |___ / _  |
 *    | | (_) | (_| |  _  | (_| |
 *    |_|\___/ \___ |_| |_|\____| */
/******************************************************************************
* [DESCRIPTION]
* Program meant to be used in a replacement memory module for Waybar. Provides
* a more precise utilization percent and hover tooltip to display processes 
* with the highest memory usage.
*
* [FILE] top_mem.c
* [LICENSE] GNU GPLv3
******************************************************************************/

// TODOs:
// - Read from a file other than /proc/$PID/status to avoid extra kernel space
// overhead. the status file has extraneous information and is around 1500 Kb, 
// an alternative source to read from would be either statm or stat.

// number has been derived emperically as status files have not been seen to 
// grow larger than 2KB in size:
// [toahd@framework13 scripts]$ cat /proc/$$/status | wc -c
// [toahd@framework13 scripts]$ 1523
#define BUFFER_SIZE 2048

// the full paths look like /proc/$PID/status
// individual $PIDs comes from the dirent stream while looping through /proc
const char PROC[] = "/proc";
const char STATUS[] = "/status";

// key type decides parsing logic when a key is found
typedef enum { KEY_LONG, KEY_STRING, NONE } key_type;

// holds the values parse from the status_keys
typedef struct {
	long vmrss;
	int pid;
	char name[40];
} proc_status;

// key struct to hold key string literal, length of key and enum hint for how
// to parse the key value
typedef struct {
	const char *key;
	const size_t key_len;
	key_type type;
	size_t offset;     // offset into target proc_status field
	size_t field_size; // size of field, such as name[size]
} status_key;
#define KEY_L(k, field) { (k), sizeof(k) - 1, KEY_LONG, \
	offsetof(proc_status, field), \
	sizeof(((proc_status*)0)->field) }
#define KEY_S(k, field) { (k), sizeof(k) - 1, KEY_STRING, \
	offsetof(proc_status, field), \
	sizeof(((proc_status*)0)->field) }
// add back to key_type enum if an INT parser is needed again
//#define KEY_I(k, field) { (k), sizeof(k) - 1, KEY_INT, \
//	offsetof(proc_status, field), \
//	sizeof(((proc_status*)0)->field) }

// full list of keys to search for in the PIDs status file. They are listed 
// below in the order that they appear in the status files. The status files
// are walked ONCE from top to bottom and loops through the keys as they are
// found. keys found that are not the current key will get skipped as a 
// consequence - so add new keys in order!
// $ cat /proc/$$/status | grep -En "Name|VmRSS|NSpid"
// 1:Name:   bash
// 14:NSpid: 321312
// 23:VmRSS: 6720 kB
static status_key status_keys[] = {
	KEY_S("Name:", name),
	//KEY_I("NSpid:", pid), optimization: just take from file name
	KEY_L("VmRSS:", vmrss),
	{NULL, 0, NONE, 0, 0}
};

// holds the top 5 memory using procs
static proc_status top[5] = {0};
#define NUM_TOPS (sizeof(top) / sizeof(top[0]))

// parses the /proc/$PID/status files for status_keys, and stores the values in
// top[] as a proc_status struct
int parse_status(int fd, proc_status *out) {
	// any bytes read from file?
	char buff[BUFFER_SIZE];
	ssize_t bytes_read = read(fd, buff, sizeof(buff) -1);
	if (bytes_read <= 0) {
		perror("Read no bytes from fd");
		return -1;
	}
	buff[bytes_read] = '\0';
	
	// start looping through status_keys
	const char *pos = buff;
	const char *end = buff + bytes_read;
	const status_key *curr_key = status_keys;
	
	while(*pos && curr_key->key != NULL) {
		if (strncmp(pos, curr_key->key, curr_key->key_len) == 0) {	
			pos += curr_key->key_len;	
			char *dest = (char *)out + curr_key->offset;
			
			if (curr_key->type == KEY_STRING) {
				while (*pos == ' ' || *pos == '\t')
					pos++;
		
				size_t i = 0;
				while(i < curr_key->field_size - 1 && *pos && *pos != '\n') {
					dest[i++] = *pos++;
				}
				dest[i] = '\0';
			} else {
				// KEY_LONG/KEY_INT logic	
				while (*pos < '0' || *pos > '9' ) {
					pos++;
				}
			
				long val = 0;
				while (*pos >= '0' && *pos <= '9') {
					val = val * 10 + (*pos++ - '0');
				}
				
				// write the correct number of bytes
				if (curr_key->type == KEY_LONG) {
					*(long *)dest = val;
				} else {
					*(int *)dest = val;
				}
			}

			curr_key++;
		}
		
		// replace this logic with memchr
		//while(*pos && *pos != '\n') pos++;
		//if (*pos == '\n') pos++;
		
		// Takes advantage of SIMD, instead of reading a single byte at
		// a time this reads in 32 bytes at a time
		const char *nl = memchar(pos, '\n', end-pos);
		if (!nl) break;
		pos = nl++;
	}

	return 1;
}

// insertion sort algorithm that builds up the top[5] array
// idx 0 holds the largest proc_status based on vmrss
void insert(const proc_status *candidate) {
	for (int i = 0; i < NUM_TOPS; i++) {
		// is the current idx smaller than the new vmrss?
		if (candidate->vmrss > top[i].vmrss) {
			// if so, shift all elements from idx backwards
			for (int j = NUM_TOPS - 1; j > i; j--) {
				top[j] = top[j - 1];
			}
			// replace the idx that was pushed back
			top[i] = *candidate;
			break;
		}
	}
}

int main() {
	// open the proc dir
	int proc_fd = open(PROC, O_RDONLY | O_DIRECTORY);
	if (proc_fd == -1) {
		perror("Failed to open proc dir");
		return 1;
	}
	
	// get a dir stream
	DIR *dir_stream = fdopendir(proc_fd);
	if (!dir_stream) {
		perror("Failed to bind to directory stream");
		close(proc_fd);
	}
	
	// loop and look for [0-9] as the first char, characteristic of a PID
	struct dirent *ent;
	while ((ent = readdir(dir_stream)) != NULL) {
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
			continue;
		}
		
		// PIDs are 
		char path[20];
		snprintf(path, sizeof(path), "%s/%s%s", PROC, ent->d_name, STATUS);
		int fd = openat(proc_fd, path, O_RDONLY);
		if (fd == -1) {
			continue;
		}
		
		// set PID field, can grab if from the dirent struct
		proc_status new_status = { .vmrss = -1, .pid = atoi(ent->d_name), .name = ""};
		if (parse_status(fd, &new_status)) {
			insert(&new_status);
		}
		close(fd);
	}

	closedir(dir_stream);

	for (size_t i = 0; i < NUM_TOPS; i++)
		printf("%7d %-16s %ld kB\n", top[i].pid, top[i].name, top[i].vmrss);
}

