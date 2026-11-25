import re, sys, collections
path, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
top = int(sys.argv[4]) if len(sys.argv) > 4 else 30
lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
head = re.compile(r'HasVK/WDDM profile: (\d+) frames in ([\d.]+) s, GPU ms/frame: passes ([\d.]+).*draws ([\d.]+); primary')
row = re.compile(r'^\s+([\d.]+) ms/frame\s+([\d.]+)/frame avg\s+([\d.]+) ms draws\s+([\d.]+) \| (.*)$')
tot = collections.defaultdict(lambda: [0.0, 0.0, 0.0])
frames = 0; passes = 0.0; draws = 0.0
cur = None; nfr = 0
for i, l in enumerate(lines, 1):
    m = head.search(l)
    if m:
        cur = first <= i <= last
        nfr = int(m.group(1))
        if cur:
            frames += nfr; passes += float(m.group(3)) * nfr; draws += float(m.group(4)) * nfr
        continue
    if not cur: continue
    m = row.match(l)
    if m:
        key = m.group(5).strip()
        t = tot[key]; t[0] += float(m.group(1)) * nfr; t[1] += float(m.group(2)) * nfr; t[2] += float(m.group(4)) * float(m.group(2)) * nfr
print(f'{frames} frames; passes {passes/frames:.2f} ms/frame, draws {draws/frames:.2f} ms/frame')
rows = [(k, t) for k, t in tot.items() if k.startswith('draw')]
shown = sum(t[0] for k, t in rows) / frames
print(f'draw buckets listed in the windows: {shown:.2f} ms/frame in {len(rows)} buckets')
# by pass
bypass = collections.defaultdict(float)
for k, t in rows:
    bypass[k.split(' :: ')[0]] += t[0] / frames
for k, v in sorted(bypass.items(), key=lambda kv: -kv[1])[:8]:
    print(f'  {v:6.2f} ms/frame | {k}')
print()
for k, t in sorted(rows, key=lambda kv: -kv[1][0])[:top]:
    print(f'{t[0]/frames:6.2f} ms/frame {t[1]/frames:6.1f}/frame avg {t[0]/max(t[1],1e-9):6.3f} ms verts {t[2]/max(t[1],1e-9):8.0f} | {k}')
