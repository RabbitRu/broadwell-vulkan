"""Write THIRD-PARTY-NOTICES.md for the binary package.

The list of files is not guessed: it is every source and header that ninja
recorded for the objects linked into vulkan_hasvk_wddm.dll and z-1.dll. The
copyright lines are collected from those files; licence texts come from the
pinned Mesa tree. Run it again after changing the Mesa version or the set of
linked sources:
    .tools\\python\\python.exe tools\\gen-third-party-notices.py
"""
import collections
import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parent.parent
build = root / 'build' / 'mesa-compiler-clang'
mesa = root / 'third_party' / 'mesa-26.2.7'
ninja = root / '.tools' / 'python' / 'Scripts' / 'ninja.exe'
TARGETS = ['src/intel/wddm_compiler/vulkan_hasvk_wddm.dll', 'subprojects/zlib-1.3.1/z-1.dll']


def run(*arguments):
    result = subprocess.run([str(ninja), '-C', str(build), *arguments], capture_output=True,
                            text=True, encoding='utf-8', errors='replace')
    if result.returncode:
        sys.exit('ninja failed: ' + result.stderr + result.stdout)
    return result.stdout


def linked_files():
    """Sources and headers of every object that goes into the shipped DLLs."""
    inputs = set()
    for target in TARGETS:
        inputs.update(line.strip() for line in run('-t', 'inputs', target).splitlines() if line.strip())
    objects = sorted(name for name in inputs if name.endswith(('.obj', '.o')))
    files = set()
    for name in inputs:
        path = (build / name).resolve()
        if path.suffix.lower() in ('.c', '.cpp', '.cc', '.h', '.hpp', '.s', '.asm') and path.is_file():
            files.add(path)
    for start in range(0, len(objects), 200):
        for line in run('-t', 'deps', *objects[start:start + 200]).splitlines():
            if line.startswith('    '):
                files.add((build / line.strip()).resolve())
    kept = []
    for path in sorted(files):
        try:
            path.relative_to(mesa)
        except ValueError:
            try:
                path.relative_to(build)
            except ValueError:
                continue                      # compiler and Windows SDK headers
        if path.is_file():
            kept.append(path)
    return kept


RULES = [
    ('BSL-1.0', re.compile(r'Boost Software License')),
    ('BSD', re.compile(r'Redistribution and use in source and binary forms')),
    ('Apache-2.0', re.compile(r'Apache License|SPDX-License-Identifier:\s*Apache-2\.0')),
    ('zlib', re.compile(r"This software is provided 'as-is'")),
    ('copyleft', re.compile(r'GNU (Lesser |Library )?General Public')),
]
COPYRIGHT = re.compile(r'Copyright\b.*', re.IGNORECASE)
SEPARATE = ('subprojects/zlib-1.3.1/', 'src/util/softfloat.', 'src/util/xxhash.h', 'src/c11/')


def copyright_lines(text):
    lines = []
    for raw in text.splitlines():
        match = COPYRIGHT.search(raw)
        if not match:
            continue
        line = match.group(0).replace('�', '(c)').replace('©', '(c)').replace('(C)', '(c)')
        line = re.sub(r'\s+', ' ', line).strip(' */.,')
        if not re.search(r'\d{4}', line):       # "copyright notice" in licence prose
            continue
        line = re.sub(r'^Copyright\s*:?\s*(\(c\)\s*)*', 'Copyright (c) ', line, flags=re.IGNORECASE)
        line = re.sub(r'\s*All [Rr]ights [Rr]eserved$', '', line).strip(' .,')
        if line.count('(') > line.count(')') and line.rfind('(') > line.find('(c)'):
            line = line[:line.rfind('(')].strip(' .,')   # a remark continued on the next line
        lines.append(line)
    return lines


def leading_comment(path):
    """The first comment block of a source file, without the comment markers."""
    text = path.read_text(encoding='utf-8', errors='replace')
    start = text.index('/*')
    block = text[start + 2:text.index('*/', start)]
    lines = [re.sub(r'^\s*\*+ ?', '', line).rstrip() for line in block.splitlines()]
    while lines and not lines[0].strip('* '):
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    return '\n'.join(lines)


def licence_text(name):
    text = (mesa / 'licenses' / name).read_text(encoding='utf-8')
    return text.split('License-Text:', 1)[1].strip('\n')


def fenced(text):
    return '```text\n' + text.rstrip() + '\n```\n'


files = linked_files()
classes = collections.defaultdict(list)
holders = collections.Counter()
for path in files:
    head = path.read_text(encoding='utf-8', errors='replace')
    found = [name for name, rule in RULES if rule.search(head[:8000])]
    relative = path.relative_to(mesa).as_posix() if mesa in path.parents else 'build/' + path.relative_to(build).as_posix()
    for name in found:
        classes[name].append(relative)
    if relative.startswith(SEPARATE):
        continue                              # quoted in full in a section of its own
    for line in set(copyright_lines(head[:8000])):
        holders[line] += 1
if classes['copyleft']:
    sys.exit('copyleft sources are linked into the driver: ' + ', '.join(classes['copyleft']))

EXPECTED = {
    'BSL-1.0': {'src/c11/impl/threads_win32.c', 'src/c11/impl/time.c', 'src/c11/threads.h'},
    'BSD': {'src/util/softfloat.c', 'src/util/softfloat.h', 'src/util/xxhash.h'},
    'zlib': {'subprojects/zlib-1.3.1/zlib.h'},
}
for name, expected in EXPECTED.items():
    extra = set(classes[name]) - expected
    if extra:
        sys.exit(f'new {name} sources are linked, describe them in this script: ' + ', '.join(sorted(extra)))
apache = sorted(classes['Apache-2.0'])
unexpected = [name for name in apache if not name.startswith(('include/vulkan/', 'include/vk_video/', 'include/GLES', 'include/KHR/'))]
if unexpected:
    sys.exit('new Apache-2.0 sources are linked, describe them in this script: ' + ', '.join(unexpected))

out = []
out.append('# Third-party notices\n')
out.append(
    'The HasVK/WDDM binary package (`vulkan_hasvk_wddm.dll`, `z-1.dll`,\n'
    '`IrisIcdProbe.exe`) contains code from the projects below. The code of this\n'
    'project itself is under the MIT License, see `LICENSE`.\n\n'
    # No file count here: it depends on the headers of the installed MSVC and SDK.
    'This file is generated by `tools/gen-third-party-notices.py` from the sources\n'
    'and headers that the build recorded for the shipped DLLs; do not edit it by hand.\n')

out.append('## Mesa 26.2.7\n')
out.append(
    'HasVK (`src/intel/vulkan_hasvk`), the ELK compiler, ISL, BLORP, NIR, the SPIR-V\n'
    'front end, the Vulkan runtime and WSI, and Mesa utility code. Source:\n'
    '<https://archive.mesa3d.org/mesa-26.2.7.tar.xz>; the changes made by this project\n'
    'are in `mesa/patches/compiler-port.patch` and `mesa/` of its repository.\n\n'
    'License: MIT. Some files word the permission notice slightly differently\n'
    '(for example with "(including the next paragraph)"); the per-file notices in the\n'
    'Mesa sources are authoritative.\n')
out.append(fenced(licence_text('MIT').replace('Copyright (c) <year> <copyright holders>\n\n', '')))
out.append('Copyright notices found in the linked Mesa sources:\n')
out.append(fenced('\n'.join(sorted(holders, key=lambda line: (line.split(' ', 3)[-1].lower(), line)))))

out.append('## Khronos Vulkan and SPIR-V headers\n')
out.append(
    'Vulkan API headers bundled with Mesa (`include/vulkan`, `include/vk_video`) are\n'
    'Copyright The Khronos Group Inc. and licensed under Apache-2.0 (most of them\n'
    '"Apache-2.0 OR MIT"); the SPIR-V headers (`src/compiler/spirv`) are under the\n'
    'Khronos MIT-style license. The text of Apache-2.0 is at the end of this file.\n')

out.append('## C11 threads emulation\n')
out.append('`src/c11` in Mesa. License: Boost Software License 1.0.\n')
out.append(fenced(leading_comment(mesa / 'src/c11/impl/threads_win32.c')))
out.append(fenced(licence_text('BSL-1.0')))

out.append('## xxHash\n')
out.append('`src/util/xxhash.h` in Mesa. License: BSD 2-Clause.\n')
out.append(fenced(leading_comment(mesa / 'src/util/xxhash.h')))

out.append('## Berkeley SoftFloat\n')
out.append('`src/util/softfloat.c` in Mesa. License: BSD 3-Clause.\n')
out.append(fenced(leading_comment(mesa / 'src/util/softfloat.c')))

out.append('## BLAKE3\n')
out.append(
    '`src/util/blake3` in Mesa, from <https://github.com/BLAKE3-team/BLAKE3>. Released\n'
    'into the public domain with CC0 1.0; alternatively licensed under Apache-2.0.\n')

out.append('## zlib 1.3.1\n')
out.append(
    '`z-1.dll`, built from the unmodified sources of\n'
    '<https://zlib.net/fossils/zlib-1.3.1.tar.gz> with the Meson build files of WrapDB.\n')
zlib = leading_comment(mesa / 'subprojects/zlib-1.3.1/zlib.h')
out.append(fenced(zlib.split('The data format used by the zlib library')[0]))

out.append('## IrisIcdProbe.exe\n')
out.append(
    'The self-test run by the installer contains JSON for Modern C++ 3.11.3\n'
    '(<https://github.com/nlohmann/json>, Copyright (c) 2013-2023 Niels Lohmann, MIT)\n'
    'and is compiled against Khronos Vulkan-Headers 1.3.301 (Apache-2.0 OR MIT).\n')

out.append('## Apache License 2.0\n')
out.append(fenced(licence_text('Apache-2.0')))

target = root / 'THIRD-PARTY-NOTICES.md'
target.write_text('\n'.join(out), encoding='utf-8', newline='\n')
print(f'{target}: {len(files)} files, {len(holders)} copyright lines')
for name in ('BSL-1.0', 'BSD', 'Apache-2.0', 'zlib'):
    print(f'  {name}: {len(classes[name])}')
