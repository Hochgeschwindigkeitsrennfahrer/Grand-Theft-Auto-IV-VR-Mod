"""Read and rewrite FusionFix's small GXT overlay (update/common/text/americanFF.gxt).

GTA IV GXT files key their strings by hash, not by name, which is why the pause
menu cannot be relabelled from XML alone. FusionFix ships this 8 KB overlay to
supply its own labels AND its own per-state value strings - "Lampposts",
"Lampposts and Headlights", ... are the four states of its Extra Night Shadows
row, which borrows the multiplayer-only MENU_DISPLAY_NETSTATS_SCORES enum.

Rewriting a string in place therefore renames that state everywhere the enum is
drawn, with no hooking and no memory patching.

Layout:
    u16 version, u16 bytesPerChar?        4 bytes
    'TKEY', u32 sizeBytes, then sizeBytes/8 x (u32 offset, u32 hash)
    'TDAT', u32 sizeBytes, then UTF-16LE NUL-terminated strings at those offsets

    python ff_gxt.py dump  <in.gxt>
    python ff_gxt.py patch <in.gxt> <out.gxt> "old text=new text" [...]
"""

import struct
import sys


def parse(path):
    with open(path, "rb") as f:
        blob = f.read()

    head = blob[:4]
    at = 4
    tkey = tdat = None
    while at < len(blob):
        tag = blob[at:at + 4]
        size = struct.unpack_from("<I", blob, at + 4)[0]
        body = blob[at + 8:at + 8 + size]
        if tag == b"TKEY":
            tkey = body
        elif tag == b"TDAT":
            tdat = body
        else:
            raise SystemExit("unexpected section %r at %d" % (tag, at))
        at += 8 + size
    if tkey is None or tdat is None:
        raise SystemExit("missing TKEY or TDAT")

    entries = []
    for i in range(0, len(tkey), 8):
        off, h = struct.unpack_from("<II", tkey, i)
        end = tdat.find(b"\x00\x00", off)
        # keep the terminator on an even boundary for UTF-16
        while end != -1 and (end - off) % 2:
            end = tdat.find(b"\x00\x00", end + 1)
        text = tdat[off:end].decode("utf-16-le") if end != -1 else ""
        entries.append([off, h, text])
    return head, entries


def build(head, entries):
    tdat = bytearray()
    tkey = bytearray()
    for e in entries:
        e[0] = len(tdat)
        tdat += e[2].encode("utf-16-le") + b"\x00\x00"
    # No padding: the shipped file's TDAT is 7466 bytes, i.e. only UTF-16 aligned.
    for off, h, _ in entries:
        tkey += struct.pack("<II", off, h)
    return (head + b"TKEY" + struct.pack("<I", len(tkey)) + bytes(tkey) +
            b"TDAT" + struct.pack("<I", len(tdat)) + bytes(tdat))


def key_hash(name):
    """GTA IV's atStringHash: Jenkins one-at-a-time over the lowercased key.

    Verified against the shipped file - oaat("skip intro") == 0xAEAE49BB, and 17
    more of the first 40 entries match, i.e. FusionFix uses the literal label as
    its own GXT key. That is why label="..." in frontend_menus.xml works at all,
    and it lets us ADD keys instead of only overwriting existing ones.
    """
    h = 0
    for ch in name.lower():
        h = (h + ord(ch)) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode = sys.argv[1]

    if mode == "add":
        src, dst = sys.argv[2], sys.argv[3]
        head, entries = parse(src)
        have = {e[1] for e in entries}
        for arg in sys.argv[4:]:
            name, _, text = arg.partition("=")
            h = key_hash(name)
            if h in have:
                for e in entries:
                    if e[1] == h:
                        print("  0x%08X  %r -> %r (existing)" % (h, e[2], text))
                        e[2] = text
            else:
                print("  0x%08X  + %r = %r" % (h, name, text))
                entries.append([0, h, text])
        with open(dst, "wb") as f:
            f.write(build(head, entries))
        print("wrote %s" % dst)
        return

    if mode == "dump":
        head, entries = parse(sys.argv[2])
        print("header %r, %d entries" % (head, len(entries)))
        for off, h, text in entries:
            print("  0x%08X  off=%-6d  %s" % (h, off, text.replace("\n", "\\n")))
        return

    if mode == "patch":
        src, dst = sys.argv[2], sys.argv[3]
        head, entries = parse(src)
        pending = []
        for arg in sys.argv[4:]:
            old, _, new = arg.partition("=")
            pending.append((old, new))
        for old, new in pending:
            hit = [e for e in entries if e[2] == old]
            if not hit:
                raise SystemExit("string not found, aborting: %r" % old)
            for e in hit:
                print("  0x%08X  %r -> %r" % (e[1], e[2], new))
                e[2] = new
        with open(dst, "wb") as f:
            f.write(build(head, entries))
        print("wrote %s" % dst)
        return

    raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
