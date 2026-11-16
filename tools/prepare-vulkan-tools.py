"""Fetch pinned Khronos cube sources and verify every extracted source file."""
import hashlib
import json
import pathlib
import subprocess
import tarfile

root = pathlib.Path(__file__).resolve().parent.parent
dependencies = json.loads((root / 'tools/dependencies.json').read_text(encoding='utf-8'))
for name in ['vulkanTools', 'vulkanHeaders', 'volk']:
    dependency = dependencies[name]
    archive = root / '.tools/downloads' / dependency['archive']
    if not archive.exists():
        subprocess.run(['curl.exe', '-fLsS', '--retry', '2', '--connect-timeout', '15', '--max-time', '120',
                        '-o', str(archive), dependency['url']], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != dependency['sha256']:
        raise SystemExit(f'Archive hash mismatch: {archive}')
    prefix = dependency['directory']
    with tarfile.open(archive, 'r:gz') as package:
        for member in package:
            relative = pathlib.PurePosixPath(member.name)
            if not relative.parts or relative.parts[0] != prefix or '..' in relative.parts or member.issym() or member.islnk():
                raise SystemExit(f'Unexpected archive member: {member.name}')
            target = root / 'third_party' / pathlib.Path(*relative.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                contents = package.extractfile(member).read()
                if target.exists():
                    if target.read_bytes() != contents:
                        raise SystemExit(f'Pinned upstream source was modified: {target}')
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(contents)
    print(f'Verified unmodified {prefix}')
