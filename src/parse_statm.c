#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "configs.h"

bool parse_statm(int proc_fd, pinfo *out, char *pid, long page_kb) {
    const char STATM[] = "/statm";

    // build up relative path to statm file
    char statm_path[PID_LENGTH + sizeof STATM]; // pid relative path buffer:
    // 1234567/statm'\0'
    snprintf(statm_path, sizeof statm_path, "%.7s%s", pid, STATM);

    int statm_fd = openat(proc_fd, statm_path, O_RDONLY);
    if (statm_fd < 0)
        return false;

    char buff[148];
    ssize_t bytes_read = read(statm_fd, buff, sizeof(buff) - 1);
    close(statm_fd);
    if (bytes_read <= 0) {
        perror("Read no bytes from status fd");
        return false;
    }
    buff[bytes_read] = '\0'; // make it a proper C string

    // pinfo keys don't matter here, in statm only the second field
    // should ever be needed and is simple to parse. fields are space
    // delimited
    const char *resident_pos = strchr(buff, ' ');
    if (resident_pos == NULL)
        return false;
    ++resident_pos;

    // the first pos is resident after the first space
    long resident_val = 0;
    while (*resident_pos >= '0' && *resident_pos <= '9') {
        resident_val = resident_val * 10 + (*resident_pos++ - '0');
    }

    // the second pos is shared after the second value gets parsed
    const char *shared_pos = ++resident_pos;

    long shared_val = 0;
    while (*shared_pos >= '0' && *shared_pos <= '9') {
        shared_val = shared_val * 10 + (*shared_pos++ - '0');
    }

    out->vmrss = resident_val * page_kb;
    out->rssanon = (resident_val - shared_val) * page_kb;

    return true;
}
