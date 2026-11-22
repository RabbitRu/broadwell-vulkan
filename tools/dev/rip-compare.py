"""Compares two rip_sampler outputs of the same binary (e.g. Talos under DX11
and under Vulkan) thread by thread, in milliseconds of a frame.

  python tools/dev/rip-compare.py a.rip.txt FPS_A b.rip.txt FPS_B [--thread N] [--pages 30] [--module Talos.exe]

The samples must come from runs with RIP_RVAS=<n> (module+rva lines; RIP_NOWALK=1
keeps the frame rate intact). A share of a thread's samples times the frame's
length is that thread's wall time per frame at that place (waits included).
--thread picks the thread by its rank in CPU use (0 = the busiest, the game's
main thread). Pages are 4 KiB of code: without symbols they still line up
between two runs of one executable.
"""
import argparse
import collections
import re


def parse(path):
    threads, cur, section = [], None, None
    raw = open(path, 'rb').read()   # Windows PowerShell redirects write UTF-16
    text = raw.decode('utf-16') if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else raw.decode('utf-8', errors='replace')
    for line in text.splitlines():
        m = re.match(r'== thread (\d+): ([\d.]+)% of one logical CPU.* (\d+) samples', line)
        if m:
            cur = {'id': int(m.group(1)), 'cpu': float(m.group(2)), 'samples': int(m.group(3)),
                   'modules': {}, 'pages': {}, 'symbols': {}}
            threads.append(cur); section = 'symbols'
            continue
        if cur is None:
            continue
        if line.startswith('   -- samples by module:'):
            for name, pct in re.findall(r' ([^\s,]+) ([\d.]+)%', line.split(':', 1)[1]):
                cur['modules'][name] = float(pct)
            continue
        if line.startswith('   -- hottest code pages'):
            section = 'pages'; continue
        if line.startswith('   -- '):
            section = None; continue
        m = re.match(r'\s+([\d.]+)% (\S.*)$', line)
        if m and section in ('pages', 'symbols'):
            cur[section][m.group(2).strip()] = float(m.group(1))
    return threads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a'); ap.add_argument('fps_a', type=float)
    ap.add_argument('b'); ap.add_argument('fps_b', type=float)
    ap.add_argument('--thread', type=int, default=0)
    ap.add_argument('--pages', type=int, default=30)
    ap.add_argument('--module', default='Talos.exe')
    args = ap.parse_args()
    ta, tb = parse(args.a), parse(args.b)
    fa, fb = 1000 / args.fps_a, 1000 / args.fps_b
    print(f'threads by CPU, % of one logical CPU (ms of CPU per frame): A frame {fa:.2f} ms, B frame {fb:.2f} ms')
    for i in range(max(len(ta), len(tb))):
        a = f'{ta[i]["cpu"]:5.1f} % ({ta[i]["cpu"] / 100 * fa:5.2f} ms)' if i < len(ta) else ''
        b = f'{tb[i]["cpu"]:5.1f} % ({tb[i]["cpu"] / 100 * fb:5.2f} ms)' if i < len(tb) else ''
        print(f'  #{i}: A {a:<20} B {b}')
    a, b = ta[args.thread], tb[args.thread]
    print(f'\nthread #{args.thread}: samples by module, ms of the thread\'s time per frame')
    names = sorted(set(a['modules']) | set(b['modules']), key=lambda n: -(a['modules'].get(n, 0) + b['modules'].get(n, 0)))
    for n in names:
        ma, mb = a['modules'].get(n, 0) / 100 * fa, b['modules'].get(n, 0) / 100 * fb
        print(f'  {n:<28} A {ma:5.2f}  B {mb:5.2f}  B-A {mb - ma:+5.2f}')
    prefix = args.module + '+'
    pages = collections.defaultdict(lambda: [0.0, 0.0])
    for key, pct in a['pages'].items():
        if key.startswith(prefix):
            pages[key][0] = pct / 100 * fa
    for key, pct in b['pages'].items():
        if key.startswith(prefix):
            pages[key][1] = pct / 100 * fb
    listed_a = sum(v[0] for v in pages.values()); listed_b = sum(v[1] for v in pages.values())
    print(f'\n{args.module} code pages of thread #{args.thread}: listed A {listed_a:.2f} ms, B {listed_b:.2f} ms of a frame')
    only_b = sum(v[1] for v in pages.values() if v[0] == 0)
    only_a = sum(v[0] for v in pages.values() if v[1] == 0)
    print(f'  pages hot only in B: {only_b:.2f} ms; only in A: {only_a:.2f} ms (a page missing from a list may still be under its cut-off)')
    # Code that runs under both: how much longer the same pages take in B. The
    # weighted median ratio is the uniform slowdown (CPU clock, a busy sibling
    # hyperthread); pages far above it do more work in B.
    common = sorted((mb / ma, ma, mb) for ma, mb in pages.values() if ma >= 0.02 and mb > 0)
    if common:
        total_a = sum(c[1] for c in common); total_b = sum(c[2] for c in common)
        acc, median = 0.0, common[-1][0]
        for ratio, ma, mb in common:
            acc += ma
            if acc >= total_a / 2:
                median = ratio; break
        over = sum(mb - ma * median for ratio, ma, mb in common if ratio > 1.5 * median)
        print(f'  pages hot in both: A {total_a:.2f} ms, B {total_b:.2f} ms (x{total_b / total_a:.2f}); weighted median ratio x{median:.2f}; '
              f'pages over 1.5x the median hold {over:.2f} ms beyond it')
    print(f'  largest differences (ms per frame):')
    for key, (ma, mb) in sorted(pages.items(), key=lambda kv: -abs(kv[1][1] - kv[1][0]))[:args.pages]:
        print(f'    {key:<24} A {ma:5.2f}  B {mb:5.2f}  B-A {mb - ma:+5.2f}')


if __name__ == '__main__':
    main()
