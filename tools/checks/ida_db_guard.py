#!/usr/bin/env python
"""IDA database guard. Two failures of IDA databases say nothing when they happen:

  1. A save or close can leave an .i64 with no Root Node netnode. It looks fine until the next open fails with
     "Database is empty" / "error 4", and a copy made after that save is no backup.
  2. The selector table can go empty while the database still opens. Every RIP-relative operand then decodes 0x10 low, so
     decompiles show wrong globals with no error. Only a running IDA can see this one.

By hand:
  python ida_db_guard.py check <file.i64> [more...]    prints OK or BROKEN per file; exit 1 if any is broken
  python ida_db_guard.py test                          the checker against a built page, no database needed
Claude Code hook modes for the IDA MCP (JSON on stdin; fail open on anything unexpected), over the databases named in
IDA_DATABASES (paths separated by ';'):
  post      PostToolUse on save_database and close_database: reads the .i64 files just written. Root Node missing: exit 2,
            with what to do on stderr.
  first     PreToolUse on execute_python: once per session and agent, hands over the selector check to run first.
  session   SessionStart: says so only if a database is already broken.

Check a copy before calling it a backup. ASCII only.
"""
import json
import mmap
import os
import struct
import sys
import tempfile
import time

PAGE = 0x2000
ID0_AT = 0xED  # an uncompressed .i64 holds the id0 here
ROOT_NAME = b"NRoot Node"
DATABASES = [p for p in os.environ.get("IDA_DATABASES", "").split(";") if p.strip()]
STATE_DIR = os.path.join(tempfile.gettempdir(), "ida-db-guard")
SELECTOR_CHECK = (
    "import idaapi, ida_segment, idautils; "
    "[hex(s) for s in idautils.Segments() if idaapi.sel2para(ida_segment.getseg(s).sel) != 0]"
)


def has_root_node(data, base=ID0_AT):
    """True or False from the B-tree; None if this is not a B-tree this code can read (a compressed .i64, say)."""
    if len(data) < base + PAGE:
        return None
    _free, page_size, root, _records, _pages = struct.unpack_from("<IHIIi", data, base)
    if page_size != PAGE or root <= 0:
        return None
    page = root
    for _ in range(64):  # the tree is far shallower than this
        at = base + page * PAGE
        if at + PAGE > len(data):
            return None
        first_child, count = struct.unpack_from("<IH", data, at)
        if 6 + 6 * count > PAGE:
            return None
        if first_child == 0:  # a leaf: keys are front-coded
            last = b""
            for i in range(count):
                indent, _unused, offset = struct.unpack_from("<HHH", data, at + 6 + 6 * i)
                length = struct.unpack_from("<H", data, at + offset)[0]
                key = last[:indent] + bytes(data[at + offset + 2 : at + offset + 2 + length])
                if key == ROOT_NAME:
                    return True
                last = key
            return False
        following = first_child
        for i in range(count):
            child, offset = struct.unpack_from("<IH", data, at + 6 + 6 * i)
            length = struct.unpack_from("<H", data, at + offset)[0]
            key = bytes(data[at + offset + 2 : at + offset + 2 + length])
            if key == ROOT_NAME:
                return True
            if key < ROOT_NAME:
                following = child
            else:
                break
        page = following
    return None


def check_file(path):
    """'ok', 'broken' or 'unknown' for one file."""
    try:
        with open(path, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            found = has_root_node(data)
    except (OSError, ValueError, struct.error):
        return "unknown"
    return "unknown" if found is None else ("ok" if found else "broken")


def broken_message(path):
    return (
        "IDA DATABASE BROKEN: %s has no Root Node. The next open fails with 'Database is empty' / error 4, and any copy of it "
        "is no backup. Stop saving, keep the broken file, and tell the user now: restore from a copy that passes "
        "'ida_db_guard.py check', or repair a copy of it." % path
    )


def recent(path, seconds):
    try:
        return time.time() - os.path.getmtime(path) < seconds
    except OSError:
        return False


def hook_post():
    json.load(sys.stdin)
    bad = [p for p in DATABASES if recent(p, 1800) and check_file(p) == "broken"]
    if bad:
        print(broken_message(bad[0]), file=sys.stderr)
        return 2
    return 0


def hook_first():
    event = json.load(sys.stdin)
    owner = "%s-%s" % (event.get("session_id", "unknown"), event.get("agent_id", "main"))
    os.makedirs(STATE_DIR, exist_ok=True)
    marker = os.path.join(STATE_DIR, "ida-selectors-%s" % "".join(c for c in owner if c.isalnum() or c == "-"))
    if os.path.exists(marker):
        return 0
    open(marker, "w").close()
    text = (
        "IDA database health, before trusting any output in this session: run this and it must return []: %s . "
        "Anything else means the selector table is lost: every RIP-relative operand and global in decompiles reads 0x10 low, with no "
        "error. Stop and tell the coordinator or the user. Never copy a database as a backup without "
        "'python tools/checks/ida_db_guard.py check <file>'." % SELECTOR_CHECK
    )
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PreToolUse", "additionalContext": text}}))
    return 0


def hook_session():
    json.load(sys.stdin)
    bad = [p for p in DATABASES if check_file(p) == "broken"]
    if bad:
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": broken_message(bad[0])}}))
    return 0


def self_test():
    def leaf(keys):
        page = bytearray(PAGE)
        struct.pack_into("<IH", page, 0, 0, len(keys))
        at = PAGE
        for i, key in enumerate(keys):
            record = struct.pack("<H", len(key)) + key + struct.pack("<H", 1) + b"v"
            at -= len(record)
            page[at : at + len(record)] = record
            struct.pack_into("<HHH", page, 6 + 6 * i, 0, 0, at)
        return bytes(page)

    def image(page_one):
        head = bytearray(PAGE)
        struct.pack_into("<IHIIi", head, 0, 0, PAGE, 1, 1, 1)
        return bytes(head) + page_one

    good = has_root_node(image(leaf([b"$ MAX NODE", ROOT_NAME, b"Nzzz"])), 0)
    gone = has_root_node(image(leaf([b"$ MAX NODE", b"Nzzz"])), 0)
    junk = has_root_node(b"\0" * (PAGE * 2), 0)
    print("root present:", good, "root gone:", gone, "not a tree:", junk)
    return 0 if good is True and gone is False and junk is None else 1


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    if mode == "check":
        worst = 0
        for path in sys.argv[2:]:
            result = check_file(path)
            print("%-8s %s" % ({"ok": "OK", "broken": "BROKEN", "unknown": "UNKNOWN"}[result], path))
            worst = worst or (1 if result == "broken" else 0)
        return worst
    if mode == "test":
        return self_test()
    try:
        return {"post": hook_post, "first": hook_first, "session": hook_session}[mode]()
    except Exception:
        return 0  # a buggy guard must never block real work


if __name__ == "__main__":
    sys.exit(main())
