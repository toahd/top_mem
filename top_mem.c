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
 *  _________
 * /__   ___/       ___       ___
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

// number has been derived emperically as status files have not been seen to 
// grow larger than 1KB in size:
// [toahd@framework13 top_mem]$ cat /proc/$$/status | wc -c
// [toahd@framework13 top_mem]$ 357
#define BUFFER_SIZE 1024

// the full paths look like /proc/$PID/stat
// individual $PIDs comes from the dirent stream while looping through /proc
const char PROC[] = "/proc";
const char STATUS[] = "/stat";

// holds the values parsed from the stat file
typedef struct {
	long vmrss;
	int pid;
	char name[40];
} proc_status;

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
	
	// first parse the comm name which is in parenthesis
	// the comm name can also CONTAIN parenthesis itself!
	// this is also the filename of the executable when launched
	const char *first_open_paren = strchr(buff, '(' );
	const char *last_closed_paren = strrchr(buff, ')' );
	
	// make sure parens exist and indexes line up
	if(!first_open_paren || !last_closed_paren || last_closed_paren < first_open_paren)
		return -1;
	
	// copy everything inbetween the parenthesis to out->name
	// same room for null terminator
	size_t name_len = (size_t)(last_closed_paren - (first_open_paren + 1));
	if (name_len >= sizeof(out->name))
		name_len = sizeof(out->name) - 1;

	memcpy(out->name, first_open_paren + 1, name_len);
	out->name[name_len] = '\0';

	// move onto parsing vmrss
	// rss is field 24, which means it follows the 22nd space after the last ')'
	int spaces = 0;
	const char *pos = last_closed_paren++;
	while (*pos && spaces < 22) {
    		if (*pos == ' ') spaces++;
    		pos++;
	}

	long rss = 0;
	while (*pos >= '0' && *pos <= '9') {
		rss = rss * 10 + (*pos++ - '0');
	}

	out->vmrss = rss;
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

	long page_size = sysconf(_SC_PAGESIZE) / 1024;   // 4096 / 1024 = 4
	for (size_t i = 0; i < NUM_TOPS; i++)
    		printf("%7d %-16s %ld kB\n", top[i].pid, top[i].name, top[i].vmrss * page_size);
}

