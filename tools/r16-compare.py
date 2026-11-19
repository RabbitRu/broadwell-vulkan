"""Compare two raw 16-bit depth dumps (1536x1536) of the same view texel by
texel: how many texels differ, by how much, and how many are covered in one
dump but empty (0, reverse Z) in the other. Writes <a>-vs-<b>.png: black =
equal, grey = small difference (<= 16), white = larger, red-free grayscale.
Usage: python -I tools/r16-compare.py <a.r16> <b.r16>"""
import struct, sys, zlib

W = H = 1536
a = struct.unpack('<%dH' % (W * H), open(sys.argv[1], 'rb').read()[:W * H * 2])
b = struct.unpack('<%dH' % (W * H), open(sys.argv[2], 'rb').read()[:W * H * 2])
same = small = large = only_a = only_b = 0
rows = []
for y in range(H):
    row = []
    for x in range(W):
        va, vb = a[y * W + x], b[y * W + x]
        if va == vb:
            same += 1; row.append(0)
        elif va == 0:
            only_b += 1; large += 1; row.append(255)
        elif vb == 0:
            only_a += 1; large += 1; row.append(255)
        elif abs(va - vb) <= 16:
            small += 1; row.append(96)
        else:
            large += 1; row.append(255)
    rows.append(row)
n = W * H
print('equal %.3f%%, differ by <=16: %.3f%%, larger: %.3f%% (covered only in A: %d texels, only in B: %d)' %
      (100.0 * same / n, 100.0 * small / n, 100.0 * large / n, only_a, only_b))


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)


out = sys.argv[1][:-4] + '-vs-' + sys.argv[2].replace('\\', '/').split('/')[-1][:-4] + '.png'
raw = b''.join(b'\x00' + bytes(r) for r in rows)
with open(out, 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
print('difference image:', out)
