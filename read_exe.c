#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "configs.h"

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
 * @*out: pointer to the pinfo struct to mutate
 *
 * @*pid: pid of the process to read /proc/$PID/comm for
 */
bool read_exe(int proc_fd, pinfo *out, const char *pid) {
    const char EXE[] = "/exe";

    // build up relative path to exe symlink
    char exe[PID_LENGTH +
             sizeof EXE]; // pid relative path buffer: 1234567/exe'\0'
    snprintf(exe, sizeof exe, "%.7s%s", pid, EXE);

    // the contents of this buffer will look something like:
    // /path/to/some/binary the max supported path length on the system should
    // be taken into account
    char buff[PATH_MAX];
    ssize_t bytes_read = readlinkat(proc_fd, exe, buff, sizeof buff - 1);
    if (bytes_read <= 0)
        return false;

    // bugfix: this was the old way to look for the ' (deleted)' postfix,
    // this is a poor choice as it matches ANY space in the buffer which
    // might not be the desired one!
    // const char *space = strrchr(buff, ' ');
    // size_t len = space ? (size_t)(space - start) : strlen(start);

    // sometimes after a system upgrade (ex: pacman -S) the original binary
    // gets deleted and a '(deleted)' postfix is added. so remove it
    static const char del[] = " (deleted)";
    static const ssize_t del_len = sizeof del - 1;
    if ((size_t)bytes_read >= del_len &&
        memcmp(buff + bytes_read - del_len, del, del_len) == 0) {
        bytes_read -= del_len;
    }
    buff[bytes_read] = '\0';

    // finds the idx of the file name starting char
    // i.e. start = 6 (the s in sudo) for: /bin/sudo
    const char *last_slash = strrchr(buff, '/');
    const char *start = last_slash ? last_slash + 1 : buff;

    // bugfix: buffer overflow was a possibility before adding this check,
    // the filename might be larger than the COMM_LEN used for out->name
    // since basenames can reach 255
    size_t len = strlen(start);

    // reserve space for null terminator
    if (len >= sizeof out->name)
        len = sizeof out->name - 1;
    memcpy(out->name, start, len);
    out->name[len] = '\0';

    return true;
}
