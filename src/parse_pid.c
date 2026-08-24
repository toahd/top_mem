#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "configs.h"

bool parse_pid(const char *name, uint32_t *pid, pinfo *status) {
    size_t pid_len = 0;
    while (name[pid_len] >= '0' && name[pid_len] <= '9' && pid_len < PID_LENGTH) {
        status->pid[pid_len] = name[pid_len];
        *pid = *pid * 10u + (uint32_t)(name[pid_len] - '0');
        ++pid_len;
    }

    // *invariant: d_name in dirent is always null terminated*
    // if the while loop was exited and the index into name is not at a
    // null terminator, then a letter was found and this is not a pid
    if (name[pid_len] != '\0')
        return false;
    status->pid[pid_len] = '\0';

    return true;
}
