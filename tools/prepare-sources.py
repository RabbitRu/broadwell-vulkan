"""Extract a pinned source archive, including file symlinks, on Windows.

Without arguments: the Mesa archive into third_party. With <dependency>
<destination>: the archive of that entry of dependencies.json (the LLVM
release, for example) into the given directory. Python reads .tar.xz by
itself; the tar.exe of older Windows builds has no xz decoder.
"""
import hashlib
import json
import pathlib
import shutil
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEPENDENCIES = json.loads((ROOT / "tools/dependencies.json").read_text())


def extract(archive, sha256, destination):
    if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != sha256:
        raise SystemExit(f"{archive.name}: SHA256 mismatch")
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as source:
        members = source.getmembers()
        # Python preserves the Unicode workspace path. Materialize file symlinks
        # without requiring Windows developer mode or symlink privileges.
        for member in members:
            if (member.isdir() or member.isfile()) and not (destination / member.name).exists():
                source.extract(member, destination, filter="data")
        directory_links = []
        for member in members:
            if not (member.issym() or member.islnk()):
                continue
            target = (destination / member.name).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise ValueError("Archive link escapes destination")
            contents = source.extractfile(member)
            if contents is None:
                directory_links.append((target, member.linkname))
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents.read())
        for target, name in directory_links:
            linked = (target.parent / name).resolve()
            if not linked.is_relative_to(destination.resolve()):
                raise ValueError("Archive directory link escapes destination")
            shutil.copytree(linked, target, dirs_exist_ok=True)


if len(sys.argv) == 3:
    dependency = DEPENDENCIES[sys.argv[1]]
    extract(ROOT / ".tools/downloads" / dependency["archive"], dependency["sha256"], pathlib.Path(sys.argv[2]))
    print(f"Extracted {dependency['archive']} to {sys.argv[2]}")
elif len(sys.argv) == 1:
    dependency = DEPENDENCIES["mesa"]
    extract(ROOT / ".tools/downloads" / f"mesa-{dependency['version']}.tar.xz", dependency["sha256"], ROOT / "third_party")
    print(f"Prepared Mesa {dependency['version']} at {ROOT / 'third_party'}")
else:
    raise SystemExit("usage: prepare-sources.py [<dependency> <destination>]")
