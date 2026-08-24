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

#include "parse_pid.c"
#include "parse_statm.c"
#include "read_comm.c"
#include "read_exe.c"

/******************************************************************************
 *  __________
 * /__   ____/      ___       ___
 *    | | ___   ____| |     __| |
 *    | |/ _ \ / _  | |___ / _  |
 *    | | (_) | (_| |  _  | (_| |
 *    |_|\___/ \___ |_| |_|\____|
 * ----------------------------------------------------------------------------
 * [DESCRIPTION]
 * Replacement memory module for Waybar. Provides a more precise utilization 
 * percent text and, in contrast to the default module, offers a tooltip that
 * displays the top processes with the highest memory usage.
 *
 * Information about the processes running on the host machine is collected 
 * from various files and then aggregated together. This module attempts to
 * provide an accurate view (while staying performant) of the systems memory 
 * usage. Some shortcuts are taken such as caching and favoring smaller/less
 * accurate files in order to meet some admittently arbitrary timing 
 * requirements.
 *
 * [FILE] top_mem.c
 * [LICENSE] GNU GPLv3
 *****************************************************************************/

// page size the kernel uses, required for for parse_statm
static long page_kb;

/* insert - insertion sort algorithm that builds up the top[5] array from zero.
 * idx 0 holds the largest pinfo based on vmrss
 *
 * @*candidate: the latest pinfo parsed from a $pid file that has its
 * vmrss checked against all other top 5 processes currently being stored
 */
void insert(pinfo *top, const pinfo *candidate) {
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

int main() {
    const char PROC[] = "/proc";
    int proc_fd = open(PROC, O_RDONLY | O_DIRECTORY);
    if (proc_fd == -1) {
        perror("Failed to open proc dir");
        return 1;
    }

    DIR *dir_stream = fdopendir(proc_fd);
    if (!dir_stream) {
        perror("Failed to bind to directory stream");
        close(proc_fd);
        return 1;
    }

    page_kb = sysconf(_SC_PAGESIZE) / 1024;

    pinfo top[NUM_TOPS] = {};
    blacklist bl = {0}; // negative cache for PIDs that fail read_exe
    whitelist wl = {0}; // cache of PIDs that pass read_exe w/ their exe name

    struct dirent *ent;
    struct timespec t_start, t_end;
    int pid_count, blacklist_hits, whitelist_hits;

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
            insert(top, &status);
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
