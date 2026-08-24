#ifndef CONFIGS_H // CONFIGS_H
#define CONFIGS_H

// how often to run main loop
// TODO: while sleeping the main logic can be run (async/threading?)
#define INTERVAL_SECS 1

// top X processes that get printed
#define NUM_TOPS 10

// expected size for a process name from the Name key in the status files
// this should equal TASK_COMM_LEN which is usually 16. Can be passed in
// if a different length is desired
#define COMM_LEN 16

// the maximum path length is directory specific, using root
// [toahd@framework13 top_mem]$ getconf PATH_MAX /
// 4096
#define PATH_MAX 4096

// [toahd@framework13 top_mem]$ wc -L < /proc/sys/kernel/pid_max
// 7
#define PID_LENGTH 7

typedef struct {
    long vmrss; // from statm: RssAnon + RssFile + RssShmem
    long rssanon; // used as the accumulator in buckets for
    // grouping processes by exe name
    char pid[PID_LENGTH + 1]; // +1 for null terminator
    char name[COMM_LEN]; // null terminator included in length
} pinfo;

#endif // CONFIGS_H
