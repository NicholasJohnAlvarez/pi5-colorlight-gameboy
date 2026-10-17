#!/usr/bin/env python3
"""
Patch gambatte-libretro's network link so the client gives up after 1 second.

Stock Gambatte calls a plain blocking connect(). If the other Game Boy (the
server) is off, the client sits through the kernel's full SYN retry budget,
about two minutes, before the game appears. That makes the exhibit
unbootable on its own. With this patch, the client tries for 1 second: if
the server is up it links, and if not it boots and plays normally. Nothing
changes between a trading day and an ordinary day.

Usage:
    python3 patch_connect_timeout.py path/to/gambatte-libretro
"""
import re
import sys
from pathlib import Path

HELPER = '''
#ifndef _WIN32
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#endif

/* Non-blocking connect with a deadline, so a missing peer costs ~1s
   instead of the kernel's full SYN retry budget (~130s). */
static int gb_connect_timeout(int fd, struct sockaddr *addr, socklen_t len, int ms)
{
#ifdef _WIN32
\treturn connect(fd, addr, len);
#else
\tint flags = fcntl(fd, F_GETFL, 0);
\tfcntl(fd, F_SETFL, flags | O_NONBLOCK);
\tint r = connect(fd, addr, len);
\tif (r == 0) { fcntl(fd, F_SETFL, flags); return 0; }
\tif (errno != EINPROGRESS) { fcntl(fd, F_SETFL, flags); return -1; }
\tfd_set wfds;
\tFD_ZERO(&wfds);
\tFD_SET(fd, &wfds);
\tstruct timeval tv;
\ttv.tv_sec  = ms / 1000;
\ttv.tv_usec = (ms % 1000) * 1000;
\tr = select(fd + 1, NULL, &wfds, NULL, &tv);
\tfcntl(fd, F_SETFL, flags);
\tif (r <= 0) { errno = ETIMEDOUT; return -1; }
\tint err = 0;
\tsocklen_t elen = sizeof(err);
\tgetsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
\tif (err != 0) { errno = err; return -1; }
\treturn 0;
#endif
}

'''


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: patch_connect_timeout.py <gambatte-libretro dir>")
    path = Path(sys.argv[1]) / "libgambatte/libretro/net_serial.cpp"
    src = path.read_text()

    if "gb_connect_timeout" in src:
        print("already patched")
        return

    anchor = "bool NetSerial::startClientSocket"
    if anchor not in src:
        sys.exit("FAIL: startClientSocket not found (different Gambatte version?)")
    src = src.replace(anchor, HELPER + anchor, 1)

    pattern = re.compile(r"connect\(\s*fd\s*,\s*\(\s*struct\s+sockaddr\s*\*\s*\)\s*&server_addr\s*,"
                         r"\s*sizeof\(\s*server_addr\s*\)\s*\)")
    src, n = pattern.subn("gb_connect_timeout(fd, (struct sockaddr *)&server_addr, sizeof(server_addr), 1000)", src)
    if n != 1:
        sys.exit(f"FAIL: expected one connect() call, found {n}")

    path.write_text(src)
    print("patched: client connect now times out after 1 s")


if __name__ == "__main__":
    main()
