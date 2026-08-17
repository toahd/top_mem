#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "blacklist.h"
#include "buckets.h"
#include "configs.h"
#include "whitelist.h"

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

// how often to run main loop
#ifndef INTERVAL_SECS
#define INTERVAL_SECS 1
#endif

// page size the kernel uses, required for for parse_statm
static long page_kb;

static pinfo top[10] = {0};
#define NUM_TOPS (sizeof(top) / sizeof(top[0]))

// forward declared
bool read_exe(int proc_fd, pinfo *out, const char *pid);
bool read_comm(int proc_fd, pinfo *out);
bool parse_pid(const char *name, uint32_t *pid, pinfo *status);
bool parse_statm(int proc_fd, pinfo *out, char *pid, long page_kb);

/* insert - insertion sort algorithm that builds up the top[5] array from zero.
 * idx 0 holds the largest pinfo based on vmrss
 *
 * @*candidate: the latest pinfo parsed from a $pid file that has its
 * vmrss checked against all other top 5 processes currently being stored
 */
void insert(const pinfo *candidate) {
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
    // open the proc dir, to limit the amount of syscalls this should be the
    // ONLY file descriptor opened
    const char PROC[] = "/proc";
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

    // blacklist for PIDs that fail read_exe
    blacklist bl = {0};

    // whitelist for PIDs that complete read_exe, caches their exe name
    whitelist wl = {0};

    struct dirent *ent;
    struct timespec t_start, t_end;
    int pid_count;
    int blacklist_hits;
    int whitelist_hits;
    while (true) {
        pid_count = 0;
        blacklist_hits = 0;
        whitelist_hits = 0;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        tick_blacklist(&bl);
        tick_whitelist(&wl);
        rewinddir(dir_stream);

        while ((ent = readdir(dir_stream)) != NULL) {
            pinfo status = {.vmrss = -1, .rssanon = -1, .pid = "", .name = ""};

            // write the pid as a char to status and get a uint32_t
            // representation
            uint32_t pid = 0;
            if (!parse_pid(ent->d_name, &pid, &status))
                continue;
            ++pid_count;

            // do not try and operate on blacklisted pids
            uint32_t bidx;
            if (is_blacklisted(&bl, pid, &bidx)) {
                ++blacklist_hits;
                continue;
            }

            // only call read_exe for non-whitelisted pids
            // can cache the contents of exe to avoid duplicate syscalls
            uint32_t widx;
            if (is_whitelisted(&wl, pid, &widx)) {
                // hit; copy the cached name out
                memcpy(status.name, wl.map[widx].comm_name, COMM_LEN);
                ++whitelist_hits;
            } else {
                // attempt to parse an exe name, if there is not an exe symlink
                // then this should be a kernel thread. so add to blacklist and
                // move on
                if (!read_exe(proc_fd, &status, ent->d_name)) {
                    blacklist_add(&bl, pid, bidx);
                    continue;
                }

                // add resulting exe comm name to white list to recall later
                // helps avoid calling readlinkat repetitively
                whitelist_add(&wl, pid, status.name, widx);
            }

            // PID may have died while being processed, so make sure this call
            // returns correctly before bucketing/inserting
            if (!parse_statm(proc_fd, &status, ent->d_name, page_kb))
                continue;

            // add to buckets before swapping the read_exe name with the
            // comm name
            add_bucket(status.name, status.rssanon);

            // Note that insert does not gaurentee an insertion!
            // insertion only happens if this proc is a top 5 contender
            insert(&status);
        }

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long delta = (t_end.tv_sec - t_start.tv_sec) * 1000000L +
                     (t_end.tv_nsec - t_start.tv_nsec) / 1000L;
        printf("time delta: %ld\n", delta);

        printf("total pid count while looping: %d\n", pid_count);
        printf("blacklist hits: %d\n", blacklist_hits);
        printf("blacklist size: %ld\n", bl.size);
        printf("whitelist hits: %d\n", whitelist_hits);
        printf("whitelist size: %ld\n", wl.size);

        // print_buckets(5);

        // for (size_t i = 0; i < NUM_TOPS; i++) {
        //     if (top[i].vmrss == 0)
        //         continue;
        //     read_comm(proc_fd, &top[i]);
        //     printf("%ld) %7s %-16s %ld kB\n", i + 1, top[i].pid, top[i].name,
        //            top[i].vmrss);
        // }

        // clear containers before looping
        memset(top, 0, sizeof top);
        clear_buckets();

        sleep(INTERVAL_SECS);
    }

    closedir(dir_stream);
}
