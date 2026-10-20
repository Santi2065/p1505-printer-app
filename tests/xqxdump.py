#!/usr/bin/env python3
"""Dump the structure of an XQX stream. Usage: xqxdump.py file.bin [--jbig outprefix]

With --jbig, writes each page as a standalone .jbg (BIH + data) so it can be
decoded with jbgtopbm for pixel comparison."""
import struct, sys

NAMES = {1: "START_DOC", 2: "END_DOC", 3: "START_PAGE", 4: "END_PAGE",
         5: "START_PLANE", 6: "END_PLANE", 7: "JBIG"}


def parse(data):
    i = data.index(b",XQX")
    pjl = data[:i]
    i += 4
    pages, cur, bih = [], None, None
    out = {"pjl": pjl.decode("latin1"), "doc": None, "pages": pages}
    while i + 8 <= len(data):
        typ, cnt = struct.unpack(">II", data[i:i + 8])
        i += 8
        if typ not in NAMES:
            out["trailer"] = data[i - 8:].decode("latin1")
            break
        if typ == 7:
            cur["jbig"] += data[i:i + cnt]
            cur["segments"].append(cnt)
            i += cnt
            continue
        items = []
        for _ in range(cnt):
            iid, size = struct.unpack(">II", data[i:i + 8])
            payload = data[i + 8:i + 8 + size]
            items.append((iid, struct.unpack(">I", payload)[0] if size == 4 else payload))
            i += 8 + size
        if typ == 1:
            out["doc"] = items
        elif typ == 3:
            cur = {"items": items, "jbig": b"", "segments": [], "planes": []}
            pages.append(cur)
        elif typ == 5:
            cur["planes"].append(items)
            for iid, v in items:
                if iid == 0x40000002:
                    cur["bih"] = v
        elif typ == 2:
            out["end"] = True
    return out


def main():
    data = open(sys.argv[1], "rb").read()
    o = parse(data)
    print("PJL:", repr(o["pjl"]))
    print("DOC:", [(hex(a), b) for a, b in o["doc"]])
    for n, p in enumerate(o["pages"], 1):
        print(f"PAGE {n}:", [(hex(a), b) for a, b in p["items"]])
        print(f"  BIH: {p['bih'].hex()}  segments={p['segments']} planes={len(p['planes'])}")
        if "--jbig" in sys.argv:
            pre = sys.argv[sys.argv.index("--jbig") + 1]
            open(f"{pre}-{n}.jbg", "wb").write(p["bih"] + p["jbig"])
    print("END_DOC:", o.get("end", False), "trailer:", repr(o.get("trailer", "")))


if __name__ == "__main__":
    main()
