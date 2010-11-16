/* io.c - light wrappers for POSIX i/o functions
 * Time-stamp: <2010-11-15 19:45:39 njk>
 *
 * (c) 2010 Nicholas J. Kain <njkain at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

/* returns -1 on error, >= 0 and equal to # chars read on success */
int safe_read(int fd, char *buf, int len)
{
    int r, s = 0;
    while (s < len) {
        r = read(fd, buf + s, len - s);
        if (r == 0)
            break;
        if (r == -1) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }
        s += r;
    }
    return s;
}

/* returns -1 on error, >= 0 and equal to # chars written on success */
int safe_write(int fd, const char *buf, int len)
{
    int r, s = 0;
    while (s < len) {
        r = write(fd, buf + s, len - s);
        if (r == -1) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }
        s += r;
    }
    return s;
}

/* returns -1 on error, >= 0 and equal to # chars written on success */
int safe_sendto(int fd, const char *buf, int len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen)
{
    int r, s = 0;
    while (s < len) {
        r = sendto(fd, buf + s, len - s, flags, dest_addr, addrlen);
        if (r == -1) {
            if (errno == EINTR)
                continue;
            else
                return -1;
        }
        s += r;
    }
    return s;
}

