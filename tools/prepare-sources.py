"""Extract the pinned Mesa archive, including file symlinks, on Windows."""
import hashlib
import json
import pathlib
import shutil
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
dependency = json.loads((ROOT / "tools/dependencies.json").read_text())["mesa"]
archive = ROOT / ".tools/downloads" / f"mesa-{dependency['version']}.tar.xz"
if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != dependency["sha256"]:
    raise SystemExit("Mesa archive SHA256 mismatch")
destination = ROOT / "third_party"
destination.mkdir(exist_ok=True)
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
print(f"Prepared Mesa {dependency['version']} at {destination}")
