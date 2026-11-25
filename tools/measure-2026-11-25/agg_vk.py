import re, sys, collections
path, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
head = re.compile(r'HasVK/WDDM profile: (\d+) frames in ([\d.]+) s, GPU ms/frame: passes ([\d.]+).*primary command buffers ([\d.]+)')
row = re.compile(r'^\s+([\d.]+) ms/frame\s+([\d.]+)/frame avg\s+([\d.]+) ms draws\s+([\d.]+) \| (.*)$')
stat = re.compile(r'per pass: ([\d.]+)k IA vertices, ([\d.]+)k prims, ([\d.]+)k VS, ([\d.]+)k clip in, ([\d.]+)k clip out, ([\d.]+)k FS')
tot = collections.defaultdict(lambda: [0.0, 0.0, 0.0, [0.0]*6, 0.0])  # ms total, count total, draws total, stats*count, stat-count
frames = 0; passes = 0.0; cmd = 0.0
cur = None; nfr = 0; last_key = None
for i, l in enumerate(lines, 1):
    m = head.search(l)
    if m:
        cur = first <= i <= last
        nfr = int(m.group(1))
        if cur:
            frames += nfr; passes += float(m.group(3)) * nfr; cmd += float(m.group(4)) * nfr
        last_key = None
        continue
    if not cur: continue
    m = row.match(l)
    if m:
        key = m.group(5).strip()
        t = tot[key]; t[0] += float(m.group(1)) * nfr; t[1] += float(m.group(2)) * nfr; t[2] += float(m.group(4)) * float(m.group(2)) * nfr
        last_key = key
        continue
    m = stat.search(l)
    if m and last_key:
        t = tot[last_key]; c = float(0)
        for k in range(6): t[3][k] += float(m.group(k + 1)) * 1e3 * 0  # placeholder
        cnt = None
        # stats are per pass averages: weight by passes in this window
        # find count for this window: recompute from row just parsed
        last_key = None
print(f'{frames} frames; passes {passes/frames:.2f} ms/frame, primary command buffers {cmd/frames:.2f} ms/frame')
for k, t in sorted(tot.items(), key=lambda kv: -kv[1][0])[:30]:
    print(f'{t[0]/frames:6.2f} ms/frame {t[1]/frames:6.1f}/frame avg {t[0]/max(t[1],1e-9):6.3f} ms draws {t[2]/max(t[1],1e-9):6.1f} | {k}')
