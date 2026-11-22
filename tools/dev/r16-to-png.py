"""Raw 16-bit depth dumps (1536x1536) -> 8-bit PNG, contrast-stretched over the
written depths; the clear value (0, reverse Z) is drawn black. Also prints how many
texels were written (not 1.0) and their depth range.
Usage: python -I r16_to_png.py <file.r16>... (PNG next to each input)"""
import struct, sys, zlib

W = H = 1536


def png(path, rows):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


for name in sys.argv[1:]:
    data = open(name, 'rb').read()
    values = struct.unpack('<%dH' % (W * H), data[:W * H * 2])
    CLEAR = 0   # reverse Z: cleared to 0
    written = [v for v in values if v != CLEAR]
    lo, hi = (min(written), max(written)) if written else (0, 1)
    span = max(hi - lo, 1)
    rows = []
    for y in range(H):
        row = values[y * W:(y + 1) * W]
        rows.append([0 if v == CLEAR else 40 + (v - lo) * 215 // span for v in row])
    png(name[:-4] + '.png', rows)
    print('%s: %.1f%% texels written, depth %d..%d' % (name.split('\\')[-1], 100.0 * len(written) / (W * H), lo, hi))
