"""Per-image timeline of one logged frame (HASVK_EXP_FRAMELOG), in submission order.
usage: framelog2.py <stdout file> <frame> [minwidth]"""
import re, sys, collections
path, frame = sys.argv[1], sys.argv[2]
minw = int(sys.argv[3]) if len(sys.argv) > 3 else 2000
lines = open(path, encoding='utf-8', errors='replace').read().splitlines()
events = collections.defaultdict(list); order = []; cur = None
for l in lines:
    m = re.match(r'FL (\d+) (submit2?) cb (\S+)', l)
    if m:
        if m.group(1) == frame and m.group(3) not in order: order.append(m.group(3))
        cur = None; continue
    m = re.match(r'FL (\d+) cb (\S+) (.*)', l)
    if m: cur = (m.group(1), m.group(2)); events[cur].append([m.group(3)]); continue
    if l.startswith('    ') and cur and l.lstrip().startswith(('col ', 'depth ', 'stencil ')): events[cur][-1].append(l.strip()); continue
    cur = None
LAY = {'0': 'UNDEF', '1': 'GENERAL', '2': 'COLOR_ATT', '3': 'DS_ATT', '4': 'DS_RO', '5': 'SHADER_RO', '6': 'XFER_SRC', '7': 'XFER_DST',
       '1000001002': 'PRESENT', '1000117000': 'D_RO_S_ATT', '1000117001': 'D_ATT_S_RO', '1000241000': 'D_ATT', '1000241001': 'D_RO',
       '1000314000': 'RO', '1000314001': 'ATT'}
LOAD = {'0': 'load', '1': 'CLEAR', '2': 'dontcare'}
img = re.compile(r'(\S+)\(f(\d+) (\d+)x(\d+) L\d+ A\d+ S\d+ u(0x[0-9a-f]+) aux(\d)( wsi)?\)')
timeline = collections.defaultdict(list); info = {}
def note(m):
    key = m.group(1)
    info[key] = f'fmt {m.group(2)} {m.group(3)}x{m.group(4)} usage {m.group(5)} aux {m.group(6)}{m.group(7) or ""}'
    return key, int(m.group(3))
for cb in order:
    for fr in sorted({k[0] for k in events}):
        ev = events.get((fr, cb)) or []
        i = 0
        while i < len(ev):
            head = ev[i][0]
            if head.startswith('pass'):
                area = head.split()[1]
                draws = '?'
                if i + 1 < len(ev) and ev[i + 1][0].startswith('endpass'): draws = ev[i + 1][0].split()[-1]
                for a in ev[i][1:]:
                    m = img.search(a)
                    if not m: continue
                    key, w = note(m)
                    role = a.split()[0]
                    lo = re.search(r'load (\d) store (\d) clear (\S+ \S+ \S+ \S+) / d (\S+) s (\S+)', a)
                    lay = re.search(r'layout (\d+)', a).group(1)
                    text = f'pass {area} as {role} [{LAY.get(lay, lay)}] {LOAD[lo.group(1)]}'
                    if lo.group(1) == '1': text += f' to ({lo.group(3)})' if role == 'col' else f' to d={lo.group(4)}' if role == 'depth' else f' to s={lo.group(5)}'
                    text += f', {draws} draws'
                    timeline[key].append(text)
            elif head.startswith(('barrier', 'copy', 'blit', 'upload', 'clear_')):
                ms = list(img.finditer(head))
                lay = re.search(r'layout (\d+) -> (\d+)', head)
                for k, m in enumerate(ms):
                    key, w = note(m)
                    if lay: timeline[key].append(f'{LAY.get(lay.group(1), lay.group(1))} -> {LAY.get(lay.group(2), lay.group(2))}')
                    else: timeline[key].append(head.split()[0] + ' ' + head.split()[1] + (' (src)' if k == 0 and len(ms) > 1 else ' (dst)'))
            i += 1
n = 0
for key, tl in timeline.items():
    w = int(re.search(r' (\d+)x', info[key]).group(1))
    if w < minw: continue
    n += 1
    print(f'=== image {n}: {info[key]}')
    prev = None; count = 0
    out = []
    for t in tl:
        if t == prev: count += 1; continue
        if prev is not None: out.append(prev + (f'  x{count}' if count > 1 else ''))
        prev = t; count = 1
    if prev is not None: out.append(prev + (f'  x{count}' if count > 1 else ''))
    for t in out: print('   ' + t)
