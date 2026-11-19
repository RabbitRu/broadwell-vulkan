"""The game's own split of the frame by sections, in ms of a frame, per API.

The Talos benchmark prints "Sections: AI=11%, physics=3%, ..." next to the
average FPS; run-talos.ps1 keeps it in build/talos/<tag>.stdout.txt. Percent of
a frame times the frame's length gives milliseconds, which compare between
APIs (percentages do not: the frames differ in length).

  python tools/talos-sections.py [--dir build/talos] [--since 2026-11-20T00:00] [--match REGEX] [--runs]

Cold runs (1 % low under 15 FPS: the game recompiles its pipelines) and runs
under a profiler (under --min-fps) are left out. The game rounds to whole
percent: one row is good to about +-0.07 ms per run.
"""
import argparse
import collections
import datetime
import pathlib
import re
import statistics


def parse(path):
    text = path.read_text(encoding='utf-8', errors='replace')
    api = re.findall(r'Gfx API: (\S+)', text)
    fps = re.findall(r'Average: ([\d.]+) FPS', text)
    low = re.findall(r'Low 1%:.*\(([\d.]+) FPS\)', text)
    sections = re.findall(r'Sections: (.*)', text)
    if not (api and fps and sections):
        return None
    parts = dict((k.strip(), float(v)) for k, v in re.findall(r'([\w ]+)=(\d+)%', sections[-1]))
    return api[-1], float(fps[-1]), float(low[-1]) if low else 0.0, parts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='build/talos')
    ap.add_argument('--since', help='only runs written after this local time (ISO)')
    ap.add_argument('--match', help='only tags matching this regular expression')
    ap.add_argument('--min-fps', type=float, default=60.0)
    ap.add_argument('--runs', action='store_true', help='list every run')
    args = ap.parse_args()
    since = datetime.datetime.fromisoformat(args.since).timestamp() if args.since else 0
    runs = collections.defaultdict(list)
    for path in sorted(pathlib.Path(args.dir).glob('*.stdout.txt'), key=lambda p: p.stat().st_mtime):
        tag = path.name[:-len('.stdout.txt')]
        if path.stat().st_mtime < since or (args.match and not re.search(args.match, tag)):
            continue
        run = parse(path)
        if not run:
            continue
        api, fps, low, parts = run
        if fps < args.min_fps or low < 15:
            continue
        frame = 1000.0 / fps
        ms = {k: v / 100 * frame for k, v in parts.items()}
        runs[api].append((tag, fps, frame, ms))
        if args.runs:
            print(f'{tag:<24} {api:<10} {fps:5.1f} FPS ' + ' '.join(f'{k} {v:.2f}' for k, v in ms.items()))
    if not runs:
        raise SystemExit('no runs')
    names = list(next(iter(runs.values()))[0][3])
    apis = sorted(runs)
    print(f'{"ms per frame":<14}' + ''.join(f'{a + " (" + str(len(runs[a])) + ")":>18}' for a in apis) +
          (f'{"difference":>14}' if len(apis) == 2 else ''))
    def row(name, pick):
        means = [statistics.mean(pick(r) for r in runs[a]) for a in apis]
        print(f'{name:<14}' + ''.join(f'{m:18.2f}' for m in means) + (f'{means[1] - means[0]:+14.2f}' if len(apis) == 2 else ''))
    for n in names:
        row(n, lambda r, n=n: r[3].get(n, 0.0))
    row('frame', lambda r: r[2])
    print(f'{"FPS":<14}' + ''.join(f'{statistics.mean(r[1] for r in runs[a]):18.1f}' for a in apis))


if __name__ == '__main__':
    main()
