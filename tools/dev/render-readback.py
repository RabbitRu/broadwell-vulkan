"""Encode the verified GPU triangle readback as PNG without changing pixels."""
import json
import pathlib
import struct
import zlib

root = pathlib.Path(__file__).resolve().parents[2]
source = root / 'build/images/257x131-y'
report = json.loads((source / 'report.json').read_text(encoding='utf-8'))
if not report.get('passed') or not report.get('texturedTriangleAndD16DepthTestValidated'):
    raise SystemExit('A passing hardware triangle report is required')
width, height = report['width'], report['height']
raw = (source / 'triangle-rgba.bin').read_bytes()
if len(raw) != width * height * 4:
    raise SystemExit('Unexpected readback size')


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


rows = b''.join(b'\0' + raw[y * width * 4:(y + 1) * width * 4] for y in range(height))
png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
       + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
target = root / 'native/artifacts/triangle-preview.png'
target.write_bytes(png)
print(target)
