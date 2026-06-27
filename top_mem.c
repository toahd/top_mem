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
#define PATH_MAX 4096

// max length of a path to a specific pid, i.e. /proc/$pid + '\0'
// pids at most can be 7 characters long
// [toahd@framework13 top_mem]$ wc -L < /proc/sys/kernel/pid_max
// 7
#define PID_LENGTH 7

// individual $PIDs comes from the dirent stream while looping through /proc
// the full paths look like /proc/$PID/status
const char PROC[] = "/proc";
const char STATUS[] = "/status";
const char EXE[] = "/exe";
const char STATM[] = "/statm";

// holds the page size the kernel uses for statm calculations
static long page_kb;

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
	//KEY_I("NSpid:", pid), optimization: take from file name instead of parsing
	KEY_L("VmRSS:", vmrss),
	{NULL, 0, NONE, 0, 0}
};

// holds the top 5 memory using procs
static proc_status top[5] = {0};
#define NUM_TOPS (sizeof(top) / sizeof(top[0]))

/*
 * parse_status - parses the /proc/$PID/status files for status_keys, and 
 * stores the values in top[] as a proc_status struct.
 *
 * @status_fd: open file descriptor to a /proc/$pid/status file
 *
 * @*out: pointer to the proc_status struct to mutate
 */
bool parse_status(int proc_fd, proc_status *out, char *pid) {
	
	// /proc is 5
	// PIDs are at most 7, but d_name has a size of 256, so truncate to 7 chars
	// /status is 7
	// + '\0'
	char status_path[PID_LENGTH + sizeof STATUS];
	snprintf(status_path, sizeof(status_path), "%.7s%s", pid, STATUS);
	
	int status_fd = openat(proc_fd, status_path, O_RDONLY);
	if (status_fd < 0) return false;

	// any bytes read from file?
	char buff[BUFFER_SIZE];
	ssize_t bytes_read = read(status_fd, buff, sizeof(buff) -1);
	close(status_fd);
	if (bytes_read <= 0) {
		perror("Read no bytes from status fd");
		return false;
	}
	buff[bytes_read] = '\0'; // make it a proper C string
	
	// start looping through status_keys
	const char *pos = buff;
	const status_key *curr_key = status_keys;
	
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

/* TODO */
bool parse_statm(int proc_fd, proc_status *out, char* pid) {
	// build up relative path to statm file
	char statm_path[PID_LENGTH + sizeof STATM]; // pid relative path buffer: 1234567/statm'\0'
	snprintf(statm_path, sizeof statm_path, "%.7s%s", pid, STATM);

	int statm_fd = openat(proc_fd, statm_path, O_RDONLY);
	if (statm_fd < 0) return false;

	char buff[148];
	ssize_t bytes_read = read(statm_fd, buff, sizeof(buff) - 1);	
	close(statm_fd);
	if (bytes_read <= 0) {
		perror("Read no bytes from status fd");
		return false;
	}
	buff[bytes_read] = '\0'; // make it a proper C string

	// proc_status keys don't matter here, in statm only the second field
	// should ever be needed and is simple to parse. fields are space
	// delimited
	const char *pos = strchr(buff, ' ') + 1;

	while(*pos != ' ') {	
		long val = 0;
		while (*pos >= '0' && *pos <= '9') {
			val = val * 10 + (*pos++ - '0');
		}
		
		out->vmrss = val * page_kb;
	}
	
	return true;
}

/* 
 * parse_exe - parses the /proc/$pid/exe symlink for the name of the program 
 * that launched the process. This is prefered over using the comm (key Name:)
 * found in the status files. As a process forks more children the comm might
 * get replaced with another name. Whereas the exe name should remain stable.
 * If an exe name is found it will take precedence over the comm name.
 *
 * For example, this effect of comm name changing can be seen with Firefox, 
 * which opens a new process per tab and has a few different names it uses such
 * as: firefox, Isolated Web, Privileged Co, etc... By using the exe name these
 * programs, which all have exe name 'firefox', can be aggregated together.
 * Otherwise, if a user has 5 tabs open at once (not uncommon) then they might
 * only see Firfox and it's related processes in the tooltip.
 *
 * @proc_fd: open file descriptor to /proc
 *
 * @*out: pointer to the proc_status struct to mutate
 */
bool parse_exe(int proc_fd, proc_status *out, const char *pid) {
	// build up relative path to exe symlink
	char exe[PID_LENGTH + sizeof EXE]; // pid relative path buffer: 1234567/exe'\0'
	snprintf(exe, sizeof exe, "%.7s%s", pid, EXE);

	// the contents of this buffer will look something like: /path/to/some/binary
	// the max supported path length on the system should be taken into account
	char buff[PATH_MAX];	
	ssize_t bytes_read = readlinkat(proc_fd, exe, buff, sizeof buff - 1);
	if (bytes_read <= 0) return false;
	// must manually append the null terminator
	buff[bytes_read] = '\0';
	
	const char *base = strrchr(buff, '/');
	const char *src = base ? base + 1 : buff;

	size_t len = strlen(src);
	if (len >= sizeof(out->name)) len = sizeof out->name - 1;
	memcpy(out->name, src, len);

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
	// open the proc dir, to limit the amount of syscalls this should be the ONLY
	// file descriptor opened
	int proc_fd = open(PROC, O_RDONLY | O_DIRECTORY);
	if (proc_fd == -1) {
		perror("Failed to open proc dir");
		return 1;
	}
	
	// get a dir stream over the /proc fd
	DIR *dir_stream = fdopendir(proc_fd);
	if (!dir_stream) {
		perror("Failed to bind to directory stream");
		close(proc_fd);
		return 1;
	}
	
	// before entering loop and parsing, get page size
	page_kb = sysconf(_SC_PAGESIZE) / 1024;

	// loop and look for [0-9] as the first char, characteristic of a PID
	struct dirent *ent;
	while ((ent = readdir(dir_stream)) != NULL) {
		// is this directory a pid? will only contain numbers
		// TODO, a process might start with a number and still not be a
		// PID that maps to a process
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
			continue;
		}
		
		// set PID field, can grab it direcly from the dirent struct
		proc_status new_status = { .vmrss = -1, .pid = atoi(ent->d_name), .name = ""};
		
		// attempt to parse an exe name
		if(parse_exe(proc_fd, &new_status, ent->d_name)) {
			// if an exe name was found then just parse statm
			// comm name is not needed and statm is much smaller
			parse_statm(proc_fd, &new_status, ent->d_name);
		} else {
			// attempt to parse memory usage and comm name if exe name was 
			// not found
			parse_status(proc_fd, &new_status, ent->d_name);
		}
		
		// Note that insert does not gaurentee an insertion, insertion
		// only happens if this is a top 5 contender
		insert(&new_status);
	}

	closedir(dir_stream);

	for (size_t i = 0; i < NUM_TOPS; i++)
		printf("%7d %-16s %ld kB\n", top[i].pid, top[i].name, top[i].vmrss);
}

