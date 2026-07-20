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

#include "buckets.h"

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
* display top processes with the highest memory usage.
*
* Information is gathered from /proc where the individual PID files inside are
* subsequently looped over. Within each pid directory the following files get
* opened, read and parsed:
*     exe - provides the name of the executable that launched the program
*     comm - provides the comm name of process
*     statm - provides memory statistics of the processes 
*
* This information is aggregated and printed out via JSON through stdin to 
* properly interface with Waybar.
*
* [FILE] top_mem.c
* [LICENSE] GNU GPLv3
******************************************************************************/

// expected size for a process name from the Name key in the status files
// this should equal TASK_COMM_LEN which is usually 16. Can be passed in
// if a different length is desired
#ifndef COMM_LEN
#define COMM_LEN 16
#endif

// the maximum path length is directory specific, using root
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
const char COMM[] = "/comm";
const char EXE[] = "/exe";
const char STATM[] = "/statm";

// global that holds the page size the kernel uses for statm calculations
static long page_kb;

// holds the values parse from the status_keys
typedef struct {
	long vmrss;   // this is: RssAnon + RssFile + RssShmem
	long rssanon; // used as the accumulator in buckets for grouping processes
		      // by exe name
	char pid[PID_LENGTH + 1]; // gets taken from file name
	char name[COMM_LEN]; // comm is 15 char max + 1 for null terminator
} proc_status;

// holds the top X memory using procs, can easily be swapped to the perfered
// number of processes to show in the rollup
static proc_status top[10] = {0};
#define NUM_TOPS (sizeof(top) / sizeof(top[0]))

/* 
 * read_exe - reads the /proc/$pid/exe symlink for the name of the program that 
 * launched the process originally. This exe name is used as an acceptable
 * trait between processes to group them together again as the parent process 
 * forks off more chidren that will like have different comm names (see 
 * read_comm() below.)
 *
 * For example, this effect of comm name changing can be seen with Firefox, 
 * which opens a new process per tab and has a few different names it uses such
 * as: firefox, Isolated Web, Privileged Co, etc... By using the exe name these
 * programs, which all have exe name 'firefox', they can be aggregated together
 * again. Otherwise, if a user has 5 tabs open at once (not uncommon) then they
 * might only see Firfox and it's related processes in the tooltip.
 *
 * @proc_fd: open file descriptor to /proc
 *
 * @*out: pointer to the proc_status struct to mutate
 *
 * @*pid: pid of the process to read /proc/$PID/comm for
 */
bool read_exe(int proc_fd, proc_status *out, const char *pid) {
	// build up relative path to exe symlink
	char exe[PID_LENGTH + sizeof EXE]; // pid relative path buffer: 1234567/exe'\0'
	snprintf(exe, sizeof exe, "%.7s%s", pid, EXE);

	// the contents of this buffer will look something like: /path/to/some/binary
	// the max supported path length on the system should be taken into account
	char buff[PATH_MAX];	
	ssize_t bytes_read = readlinkat(proc_fd, exe, buff, sizeof buff - 1);
	if (bytes_read <= 0) return false;

	// bugfix: this was the old way to look for the ' (deleted)' postfix,
	// this is a poor choice as it matches ANY space in the buffer which
	// might not be the desired one!
	//const char *space = strrchr(buff, ' ');
	//size_t len = space ? (size_t)(space - start) : strlen(start);

	// sometimes after a system upgrade (ex: pacman -S) the original binary gets 
	// deleted and a '(deleted)' postfix is added. so remove it
	static const char del[] = " (deleted)";
	const size_t del_len = sizeof del - 1;
	if((size_t)bytes_read >= del_len && memcmp(buff + bytes_read - del_len, del, del_len) == 0) {
		bytes_read -= del_len;
	}
	buff[bytes_read] = '\0';

	// this finds the idx of the file name starting char
	// i.e. start = 6 (the s in sudo) for: /bin/sudo
	const char *last_slash = strrchr(buff, '/');
	const char *start = last_slash ? last_slash + 1 : buff;
	
	// bugfix: buffer overflow was a possibility before adding this check,
	// the filename might be larger than the COMM_LEN used for out->name
	// since basenames can reach 255
	size_t len = strlen(start);
	if (len >= sizeof out->name) len = sizeof out->name - 1;
	memcpy(out->name, start, len);
	out->name[len] = '\0';

	return true;
}

/* read_comm - Reads the comm name from the /proc/$PID/comm file and sets the
 * name member in the proc_status passed in. The comm name found within the 
 * comm file differs from the name found in /proc/$PID/exe in that is is used
 * as the indidivual PID name (for use in the top X procs) as opposed to the
 * bucket name that is used which comes from the /proc/$PID/exe symlink.
 *
 * @proc_fd: open file descriptor to /proc
 *
 * @*out: pointer to the proc_status struct to mutate
 *
 * @*pid: pid of the process to read /proc/$PID/comm for
 */
bool read_comm(int proc_fd, proc_status *out) {
	char comm_path[PID_LENGTH + sizeof COMM];
	snprintf(comm_path, sizeof comm_path, "%.7s%s", (char*)out->pid, COMM);

	int comm_fd = openat(proc_fd, comm_path, O_RDONLY);
	if (comm_fd < 0) return false;

	ssize_t bytes_read = read(comm_fd, out->name, sizeof(out->name));
	close(comm_fd);
	if (bytes_read <= 0) {
		perror("Read no bytes from comm fd");
		return false;
	}

	// replace the newline in the output of comm with a terminator
	// the last byte will always be a newline
	out->name[bytes_read - 1] = '\0';
	
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
	const char *resident_pos = strchr(buff, ' ');
	if (resident_pos == NULL) return false;
	++resident_pos;

	// the first pos is resident after the first space
	long resident_val = 0;
	while (*resident_pos >= '0' && *resident_pos <= '9') {
		resident_val = resident_val * 10 + (*resident_pos++ - '0');
	}

	// the second pos is shared after the second value gets parsed
	const char* shared_pos = ++resident_pos;
	
	long shared_val = 0;
	while (*shared_pos >= '0' && *shared_pos <= '9') {
		shared_val = shared_val * 10 + (*shared_pos++ - '0');
	}
		
	out->vmrss = resident_val * page_kb;
	out->rssanon = (resident_val - shared_val) * page_kb;
	
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

/* main - Contians the core logic to open the /proc dir and loop over all of 
 * the files inside, decide what's a process and hand off the /proc fd and
 * current pid to the parse functions to gather statistics.
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
		// PID that maps to a process...
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
			continue;
		}
		
		// set PID field, can grab it direcly from the dirent struct
		proc_status status = { .vmrss = -1, .rssanon = -1, .pid = "", .name = ""};
		snprintf(status.pid, sizeof status.pid, "%.7s", ent->d_name);
		
		// attempt to parse an exe name, if there is not an exe symlink then this
		// should be a kernel thread ...so leave!
		if (!read_exe(proc_fd, &status, ent->d_name)) continue;
			
		// attempt to parse memory usage and comm name if exe name was 
		// not found
		//parse_status(proc_fd, &new_status, ent->d_name);
		
		parse_statm(proc_fd, &status, ent->d_name);
		// add to buckets before swapping the read_exe name with the
		// comm name
		add_bucket(status.name, status.rssanon);
		
		// Note that insert does not gaurentee an insertion!
		// insertion only happens if this proc is a top 5 contender
		insert(&status);
	}

	print_buckets(5);

	for (size_t i = 0; i < NUM_TOPS; i++) {
		if (top[i].vmrss == 0) continue;
		read_comm(proc_fd, &top[i]);
		printf("%ld) %7s %-16s %ld kB\n", i+1, top[i].pid, top[i].name, top[i].vmrss);
	}
	
	closedir(dir_stream);
}

