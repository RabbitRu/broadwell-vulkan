"""Decodes Gen8 RENDER_SURFACE_STATE candidates around a matched DW2 (height/width).
usage: surf_state.py <probe stdout file>   (lines 'kernel candidate <file>: address ... (match at +N)')"""
import re, struct, sys, collections
seen = collections.Counter()
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'kernel candidate (\S+-find-\d+\.bin): address (\S+) \(match at \+(\d+)\)', line)
    if not m: continue
    data = open(m.group(1), 'rb').read()
    at = int(m.group(3))
    if at < 8 or at + 4 * 11 > len(data): continue
    dw = struct.unpack_from('<16I', data + b'\0' * 64, at - 8)
    stype = dw[0] >> 29; fmt = (dw[0] >> 18) & 0x1ff; tile = (dw[0] >> 12) & 3
    valign = (dw[0] >> 16) & 3; halign = (dw[0] >> 14) & 3
    mocs = (dw[1] >> 24) & 0x7f; qpitch = dw[1] & 0x7fff
    h = ((dw[2] >> 16) & 0x3fff) + 1; w = (dw[2] & 0x3fff) + 1
    pitch = (dw[3] & 0x3ffff) + 1
    # plausibility: 2D surface (type 1), sane pitch
    if stype != 1 or pitch < w or pitch > w * 16: continue
    key = (fmt, tile, mocs, w, h, pitch, qpitch, valign, halign, dw[0] & 0xfff, dw[4], dw[5], dw[6], dw[7])
    seen[key] += 1
for key, n in seen.most_common():
    fmt, tile, mocs, w, h, pitch, qpitch, valign, halign, low, d4, d5, d6, d7 = key
    print(f'{n:3d}x  format 0x{fmt:03x} tile {tile} MOCS 0x{mocs:02x} {w}x{h} pitch {pitch} qpitch {qpitch} valign {valign} halign {halign} '
          f'dw0.low 0x{low:03x} dw4 0x{d4:08x} dw5 0x{d5:08x} dw6 0x{d6:08x} dw7 0x{d7:08x}')
if not seen: print('no plausible surface states')
