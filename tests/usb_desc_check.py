#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# The USB configuration descriptor as built (build/zp12.bin), walked as a host does: every descriptor's length
# adds up to wTotalLength, the interfaces counted, the audio control header's total and collection, the USB
# audio output for sampling (ZP12_UAI): IF2 alt 0 / 1, PCM 2 ch 16 bit 44.1 kHz, EP2 OUT isochronous adaptive.
#   python3 tests/usb_desc_check.py [build/zp12.bin]
import struct
import sys

img = open(sys.argv[1] if len(sys.argv) > 1 else "build/zp12.bin", "rb").read()
bad = 0


def check(ok, what):
    global bad
    print("usb: %-66s %s" % (what, "ok" if ok else "FAIL"))
    bad += not ok


def walk(at):
    total = struct.unpack_from("<H", img, at + 2)[0]
    out, i = [], at
    while i < at + total:
        n = img[i]
        if n < 2:
            return None
        out.append(img[i:i + n])
        i += n
    return out if i == at + total else None


found = []
for at in range(len(img) - 9):
    if img[at] == 9 and img[at + 1] == 2 and img[at + 5] == 1 and 60 < struct.unpack_from("<H", img, at + 2)[0] < 400:
        d = walk(at)
        if d:
            found.append(d)
check(len(found) == 1, "one configuration descriptor in the image")
d = found[0]
cfg = d[0]
ifs = [x for x in d if x[1] == 4]
check(len({x[2] for x in ifs}) == cfg[4], "bNumInterfaces = the interfaces there (%d)" % cfg[4])
hdr = [x for x in d if x[1] == 0x24 and x[2] == 1 and len(x) >= 8 and x[3:5] == b"\x00\x01"]
ac = hdr[0]
i0 = d.index(ac)
cs = 0
for x in d[i0:]:
    if x[1] != 0x24:
        break
    cs += x[0]
check(struct.unpack_from("<H", ac, 5)[0] == cs, "AC header wTotalLength = its class descriptors (%d)" % cs)
coll = list(ac[8:8 + ac[7]])
check(coll == [1, 2], "AC header: MIDI streaming (1) and audio streaming (2)")
it = [x for x in d if x[1] == 0x24 and x[2] == 2 and x[0] == 12][0]
ot = [x for x in d if x[1] == 0x24 and x[2] == 3 and x[0] == 9 and x[3] == 2][0]
check(struct.unpack_from("<H", it, 4)[0] == 0x0101 and it[7] == 2, "input terminal: USB streaming, 2 channels")
check(struct.unpack_from("<H", ot, 4)[0] == 0x0301 and ot[7] == it[3], "output terminal: speaker, from the input terminal")
as_if = [x for x in ifs if x[2] == 2]
check([x[3] for x in as_if] == [0, 1] and [x[4] for x in as_if] == [0, 1], "IF2: alt 0 without, alt 1 with an endpoint")
fmt = [x for x in d if x[1] == 0x24 and x[2] == 2 and x[0] == 11][0]
rate = fmt[8] | fmt[9] << 8 | fmt[10] << 16
check(fmt[3] == 1 and fmt[4] == 2 and fmt[5] == 2 and fmt[6] == 16 and rate == 44100, "format: PCM 2 ch, 16 bit, 44100 Hz")
eps = [x for x in d if x[1] == 5]
ep = [x for x in eps if x[2] == 0x02][0]
maxp = struct.unpack_from("<H", ep, 4)[0]
check(ep[3] == 0x09 and maxp >= 45 * 4 and ep[6] == 1, "EP2 OUT isochronous adaptive, 1 ms, room for 45 frames (%d B)" % maxp)
check(len({x[2] for x in eps}) == len(eps), "no endpoint address twice")
print("usb: %s" % ("FAILED" if bad else "all ok"))
sys.exit(bad != 0)
