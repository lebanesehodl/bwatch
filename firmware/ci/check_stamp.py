#!/usr/bin/env python3
"""Check that a firmware image carries the build stamp About shows.

    check_stamp.py IMAGE TEXT
        IMAGE must hold TEXT ("FW 1a2b3c4", "DEV 1a2b3c4" or "FW DEV") as a
        C string exactly once, and no other build stamp. Exit 1 otherwise.

    check_stamp.py --mismatch-control IMAGE TEXT
        The check's own control: copies IMAGE with its stamp changed to name
        another commit, and requires the check above to FAIL on that copy.
        Exit 1 if the check would have passed it.

build.sh runs both on every build (see FW_ID in the sketch).
"""
import re
import sys

STAMP = re.compile(rb"(?:FW|DEV) (?:[0-9a-f]{7}|DEV)\x00")


def problems(data, text):
    want = text.encode() + b"\x00"
    found = STAMP.findall(data)
    out = []
    if found.count(want) != 1:
        out.append("%r appears %d times, not once" % (text, found.count(want)))
    others = sorted(set(s for s in found if s != want))
    if others:
        out.append("other build stamps: %s" % ", ".join(repr(s[:-1].decode()) for s in others))
    return out


def main(argv):
    if len(argv) == 4 and argv[1] == "--mismatch-control":
        path, text = argv[2], argv[3]
        data = open(path, "rb").read()
        m = re.fullmatch(r"(FW|DEV) ([0-9a-f]{7})", text)
        if not m:
            print("stamp control: %r has no commit to change" % text)
            return 1
        sha = m.group(2)
        other = sha[:-1] + ("0" if sha[-1] != "0" else "1")   # another commit
        wrong = (m.group(1) + " " + other).encode() + b"\x00"
        right = text.encode() + b"\x00"
        if data.count(right) != 1:
            print("stamp control: %r is not in the image once" % text)
            return 1
        bad = data.replace(right, wrong)
        p = problems(bad, text)
        if not p:
            print("stamp control FAILED: an image stamped %r passed as %r" % (wrong[:-1].decode(), text))
            return 1
        print("stamp control: an image stamped %r is refused (%s)" % (wrong[:-1].decode(), "; ".join(p)))
        return 0
    if len(argv) != 3:
        print(__doc__)
        return 2
    path, text = argv[1], argv[2]
    p = problems(open(path, "rb").read(), text)
    if p:
        print("stamp check FAILED for %s: %s" % (path, "; ".join(p)))
        return 1
    print("stamp check: %s holds %r once" % (path.rsplit("/", 1)[-1], text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
