#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "download.h"

int cb_state_write(const char *path, const void *data, size_t len)
{
    char tmp[1024];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    const char *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    close(fd);
    return rename(tmp, path) == 0 ? 0 : -1;
}

long cb_state_read(const char *path, void *buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char *)buf + got, len - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    return (long)got;
}
