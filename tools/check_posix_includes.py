#!/usr/bin/env python3
"""Checks that each C++ file's POSIX branch declares the POSIX-only symbols it uses.

Why this exists: a local build on Windows compiles the `_WIN32` branch and never the `#else` one, so
a missing POSIX header is invisible until CI runs it. That has bitten twice, in two different files:
`stream.cpp` was missing `<netdb.h>`, and then `agent.cpp` gained POSIX code later and needed its own
copy. Both times the symptom was a CI compile error minutes away and both times it was preventable by
looking.

This cannot prove the POSIX branch compiles -- only a POSIX compiler can do that, and CI is that. What
it catches is the specific, mechanical, twice-seen mistake: a POSIX-only symbol used in a file whose
POSIX branch does not include the header that declares it.

    python tools/check_posix_includes.py          # check; exit 1 on any gap
"""

import pathlib
import re
import sys

# POSIX-only symbol -> the header that declares it. Narrow on purpose: only what this project uses.
NEEDS = {
    "addrinfo": "netdb.h",
    "getaddrinfo": "netdb.h",
    "freeaddrinfo": "netdb.h",
    "gethostname": "unistd.h",
    "getpeername": "sys/socket.h",
    "accept": "sys/socket.h",
    "close": "unistd.h",
    "inet_ntop": "arpa/inet.h",
    "fcntl": "fcntl.h",
    "poll": "poll.h",
    "socklen_t": "sys/socket.h",
    "sockaddr_in": "netinet/in.h",
    "S_IRUSR": "sys/stat.h",
    "S_IWUSR": "sys/stat.h",
    "chmod": "sys/stat.h",
}

SOURCES = sorted(pathlib.Path("agent/src").glob("*.cpp"))

INCLUDE_RE = re.compile(r"#include\s+<([^>]+)>")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")


def strip_comments(text):
    """Removes C++ comments so a commented-out include is not counted as a real one.

    Without this the checker cannot detect the thing it exists to detect. Commenting out
    `#include <netdb.h>` to test it leaves the header name in the text, the scanner matches it, and the
    file passes -- verified, and it is exactly how this check was wrong on its first run. A check that
    fails open is worse than no check, because it reports green.
    """
    return LINE_COMMENT_RE.sub(" ", BLOCK_COMMENT_RE.sub(" ", text))


def posix_includes(text):
    """Every include that is live on POSIX, from either shape of platform guard.

    Two shapes count. In `#ifdef _WIN32 ... #else ... #endif` it is the `#else` half, and
    `#ifndef _WIN32 ... #endif` counts whole. The second shape is how `credential.cpp` guards
    `<sys/stat.h>`, and a checker that understood only the first reported it as missing a header it
    plainly has -- a false positive that would have taught everyone to ignore this output.
    """
    found = set()
    for match in re.finditer(r"#ifdef _WIN32\n(.*?)#else\n(.*?)#endif", text, re.S):
        found.update(INCLUDE_RE.findall(match.group(2)))
    for match in re.finditer(r"#ifndef _WIN32\n(.*?)#endif", text, re.S):
        found.update(INCLUDE_RE.findall(match.group(1)))
    return found


def main():
    problems = []
    checked = 0
    for path in SOURCES:
        text = strip_comments(path.read_text(encoding="utf-8"))
        includes = posix_includes(text)
        if not includes:
            print("  {:<18} -- no POSIX guard, nothing to check".format(path.name))
            continue
        checked += 1
        missing = sorted(
            "{} (needs <{}>)".format(symbol, header)
            for symbol, header in NEEDS.items()
            if re.search(r"\b" + re.escape(symbol) + r"\b", text) and header not in includes
        )
        if missing:
            problems.append(path.name)
            print("  {:<18} MISSING {}".format(path.name, ", ".join(missing)))
        else:
            print("  {:<18} ok  ({})".format(path.name, ", ".join(sorted(includes))))

    print()
    if problems:
        print("FAIL: {} file(s) use a POSIX symbol with no header declaring it: {}".format(
            len(problems), ", ".join(problems)))
        print("A Windows build cannot see this -- it compiles the _WIN32 branch instead.")
        return 1
    print("posix include check OK ({} file(s) with a POSIX guard)".format(checked))
    print("note: this checks the includes, not that the branch compiles. CI is that.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
