#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include "configs.h"

/* read_comm - Reads the comm name from the /proc/$PID/comm file and sets the
 * name member in the pinfo passed in. The comm name found within the
 * comm file differs from the name found in /proc/$PID/exe in that is is used
 * as the indidivual PID name (for use in the top X procs) as opposed to the
 * bucket name that is used which comes from the /proc/$PID/exe symlink. *
 * @proc_fd: open file descriptor to /proc
 *
 * @*out: pointer to the pinfo struct to mutate
 *
 * @*pid: pid of the process to read /proc/$PID/comm for
 */
bool read_comm(int proc_fd, pinfo *out) {
    const char COMM[] = "/comm";

    char comm_path[PID_LENGTH + sizeof COMM];
    snprintf(comm_path, sizeof comm_path, "%.7s%s", (char *)out->pid, COMM);

    int comm_fd = openat(proc_fd, comm_path, O_RDONLY);
    if (comm_fd < 0)
        return false;
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
