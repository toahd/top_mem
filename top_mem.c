#include <stdio.h>
#include <stdbool.h>
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
* Small monolithic C program designed to be used in a replacement memory module
* for Waybar. Provides a more precise utilization percent text and a tooltip to
* display the top processes with the highest memory usage.
*
* Statistics are gathered from the /proc virtual filesystem and the individual
* PID files inside are subsequently looped over. Within each /proc/<pid> 
* directory both the status and exe files are parsed for information such as
* VmRSS and the executable name that is responsible for launching the program.
*
* This information is aggregated and printed out via JSON through stdin to 
* properly interface with Waybar.
*
* [FILE] top_mem.c
* [LICENSE] GNU GPLv3
******************************************************************************/

// buffer size for status files. This value has been derived emperically as
// status files have not been seen to grow larger than 2KB in size:
// [toahd@framework13 top_mem]$ cat /proc/$$/status | wc -c
// [toahd@framework13 top_mem]$ 1523
#define BUFFER_SIZE 2048

// expected size for a process name from the Name key in the status files
// this should equal TASK_COMM_LEN which is usually 16
#ifndef COMM_LEN
#define COMM_LEN 16
#endif

// the maximum path length is directory specific, 
// [toahd@framework13 top_mem]$ getconf PATH_MAX /
// 4096
#define EXE_PATH_MAX 4096

// max length of a path to status, i.e. /proc/$pid/status + '\0'
// pids at most can be 7 characters long
// [toahd@framework13 top_mem]$ wc -L < /proc/sys/kernel/pid_max
// 7
#define STATUS_PATH_MAX 21

// individual $PIDs comes from the dirent stream while looping through /proc
// the full paths look like /proc/$PID/status
const char PROC[] = "/proc";
const char STATUS[] = "/status";

// key type decides parsing logic when a key is found
typedef enum { KEY_LONG, KEY_STRING, NONE } key_type;

// holds the values parse from the status_keys
typedef struct {
	long vmrss;
	int pid;
	char name[COMM_LEN]; // comm is 15 char max, +1 for null terminator
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

// full list of keys to search for in the PIDs status file. They are listed 
// below in the order that they appear in the status files. The status files
// are walked ONCE from top to bottom and loops through the keys as they are
// found. keys found that are not the current key will get skipped as a 
// consequence - so add new keys in order!
// [toahd@framework13 top_mem]$ cat /proc/$$/status | grep -En "Name|VmRSS|NSpid"
// 1:Name:   bash
// 14:NSpid: 321312
// 23:VmRSS: 6720 kB
static status_key status_keys[] = {
	KEY_S("Name:", name),
	//KEY_I("NSpid:", pid), optimization: take from file name, no parsing
	KEY_L("VmRSS:", vmrss),
	{NULL, 0, NONE, 0, 0}
};

// holds the top 5 memory using procs
static proc_status top[5] = {0};
#define NUM_TOPS (sizeof(top) / sizeof(top[0]))

/*
 * parse_status - parses the /proc/$PID/status files for status_keys, and 
 * stores the values in top[] as a proc_status struct
 *
 * @status_fd: open file descriptor to a /proc/$pid/status file
 *
 * @*out: pointer to the proc_status struct to mutate
 */
bool parse_status(int status_fd, proc_status *out, bool has_exe) {
	// any bytes read from file?
	char buff[BUFFER_SIZE];
	ssize_t bytes_read = read(status_fd, buff, sizeof(buff) -1);
	if (bytes_read <= 0) {
		perror("Read no bytes from status fd");
		return false;
	}
	buff[bytes_read] = '\0'; // make it a proper C string
	
	// start looping through status_keys
	const char *pos = buff;
	const status_key *curr_key = status_keys;
	
	// if a exe name was found, skip comm name
	// this assumes 'Name:' always is in spot one of status_keys!
	if(has_exe) curr_key++;
	
	while(*pos && curr_key->key != NULL) {
		if (strncmp(pos, curr_key->key, curr_key->key_len) == 0) {	
			pos += curr_key->key_len;	
			char *dest = (char *)out + curr_key->offset;
			
			if (curr_key->type == KEY_STRING) {
				while (*pos == '\t' || *pos == ' ')
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
				*(long *)dest = val;
			}

			curr_key++;
		}
		
		// replace this logic with memchr
		//while(*pos && *pos != '\n') pos++;
		//if (*pos == '\n') pos++;
		
		// Takes advantage of SIMD, instead of reading a single byte at
		// a time this reads in 32 bytes at a time
		const char *end = buff + bytes_read;
		const char *nl = memchr(pos, '\n', end - pos);
		if (!nl) break;
		pos = nl + 1;
	}

	return true;

}

/* 
 * parse_exe - parses the /proc/$pid/exe files for the name of the program that
 * launched the process. This is prefered over using the comm (key Name:) found
 * in the status files. As a process forks more children the comm might get 
 * replaced with another name, where the exe name should remain stable. The exe
 * name is perfered to the comm name and if found will take precedence over the
 * comm name.
 *
 * This can be seen with Firefox, which opens a new process per tab and has a
 * few different names it uses like: firefox, Isolated Web, Privileged Co, etc.
 * By using the exe name these programs, which all have exe name 'firefox', 
 * can be aggregated together. Otherwise, if a user has 5 tabs open at once
 * (not uncommon) then they might only see Firfox and it's related processes in
 * the tooltip.
 *
 * It is for this reason that the exe name is perfered to the comm name and if 
 * found will take precedence.
 *
 * @exe_fd: open file descriptor to a /proc/$pid/exe file
 *
 * @*out: pointer to the proc_status struct to mutate
 *
 * @pid: pid of the process to parse exe for
 *
 */
bool parse_exe(int exe_fd, proc_status *out, const char *pid) {
	char link[32], exe_path[EXE_PATH_MAX];
	
	// build up the name of the symlink, when using a relative link path
	// the link is resolved relative to the directory fd
	snprintf(link, sizeof(link), "%.15s/exe", pid);
	ssize_t bytes_read = readlinkat(exe_fd, link, exe_path, sizeof exe_path - 1);
	if (bytes_read <= 0) return -1;
	// must manually append the null terminator
	exe_path[bytes_read] = '\0';
	
	const char *base = strrchr(exe_path, '/');
	const char *src = base ? base + 1 : exe_path;

	size_t len = strlen(src);
	if (len >= sizeof(out->name)) len = sizeof out->name - 1;
	memcpy(out->name, src, len);

	printf("%s\n", out->name);

	return true;

}

/* insert - insertion sort algorithm that builds up the top[5] array from zero.
 * idx 0 holds the largest proc_status based on vmrss
 *
 * @*candidate: the latest proc_status parsed from a $pid file that has its 
 * vmrss checked against all other top 5 processes currently being stored
 */
void insert(const proc_status *candidate) {
	for (size_t i = 0; i < NUM_TOPS; i++) {
		// is the current idx smaller than the new vmrss?
		if (candidate->vmrss > top[i].vmrss) {
			// if so, shift all elements from idx backwards
			for (size_t j = NUM_TOPS - 1; j > i; j--) {
				top[j] = top[j - 1];
			}
			// replace the idx that was pushed back
			top[i] = *candidate;
			break;
		}
	}
}

/* main - contians the logic to open the /proc dir and loop over all of the
 * processes to hand off file descriptors/pids to the parse functions. main
 * handles closing the file descriptors.
 */
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
		return 1;
	}
	
	// loop and look for [0-9] as the first char, characteristic of a PID
	struct dirent *ent;
	while ((ent = readdir(dir_stream)) != NULL) {
		// is this directory a pid? will only contain numbers
		// TODO, a process might start with a number that's not a PID
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
			continue;
		}
		
		// /proc is 5
		// PIDs are at most 7, but d_name has a size of 256, so truncate to 7 chars
		// /status is 7
		// + '\0'
		char status_path[STATUS_PATH_MAX];
		snprintf(status_path, sizeof(status_path), "%s/%.7s%s", PROC, ent->d_name, STATUS);
		printf("status path: %s\n", status_path);
		int fd = openat(proc_fd, status_path, O_RDONLY);
		if (fd == -1) {
			perror("Could not open pid dir");
			continue;
		}
		
		// set PID field, can grab it direcly from the dirent struct
		proc_status new_status = { .vmrss = -1, .pid = atoi(ent->d_name), .name = ""};
		
		// attempt to parse an exe name
		bool has_exe = parse_exe(proc_fd, &new_status, ent->d_name);
		
		// attempt to parse memory usage and comm name if exe name was 
		// not found
		if (parse_status(fd, &new_status, has_exe)) {
			insert(&new_status);
		}
		close(fd);
	}

	closedir(dir_stream);

	for (size_t i = 0; i < NUM_TOPS; i++)
		printf("%7d %-16s %ld kB\n", top[i].pid, top[i].name, top[i].vmrss);
}

