"""Fetch the x64 compiler and resource headers from Microsoft's pinned VSIX.

The complete VSIX includes three architectures and is over 2 GiB. HTTP byte
ranges avoid the VS Installer's failing download path and unnecessary tools.
No Visual Studio installation files are changed by this script.
"""
import concurrent.futures
import hashlib
import json
import pathlib
import struct
import subprocess
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
URL = "https://download.visualstudio.microsoft.com/download/pr/e98d75fa-91b1-47a1-9cb7-b6556de592c5/3f4c2578467784756f3706848d8ebec3f0b1f6705c062b2be769f4138534115f/payload.vsix"
CACHE = ROOT / ".tools/downloads/clang-ranges"
INSTALL = ROOT / ".tools/clang"
CACHE.mkdir(parents=True, exist_ok=True)


def fetch(start, size, name):
    path = CACHE / name
    if not path.exists() or path.stat().st_size != size:
        subprocess.run([
            "curl.exe", "-fLsS", "--retry", "2", "--connect-timeout", "15",
            "--max-time", "120", "--max-filesize", str(size),
            "--range", f"{start}-{start + size - 1}", "-o", str(path), URL,
        ], check=True)
    contents = path.read_bytes()
    if len(contents) != size:
        raise ValueError("HTTP server did not honor byte range")
    return contents


directory = fetch(2224315662, 119303, "directory.bin")
if hashlib.sha256(directory).hexdigest() != "8aac434b3d0d8c4cbdbf9bcda6663d0f2cf18f1d76168ef77cc694ca57ad0046":
    raise SystemExit("Pinned Clang VSIX directory SHA256 mismatch")
offset = 0
entries = []
prefix = "Contents/VC/Tools/Llvm/x64/"
while offset < len(directory):
    header = struct.unpack_from("<4s6H3I5H2I", directory, offset)
    if header[0] != b"PK\x01\x02":
        raise ValueError("Invalid ZIP central directory")
    name = directory[offset + 46:offset + 46 + header[10]].decode("utf-8")
    if name.startswith(prefix):
        relative = name[len(prefix):]
        selected = relative.startswith("lib/clang/19/include/") or relative in {
            "bin/clang-cl.exe", "bin/lld-link.exe", "bin/llvm-lib.exe",
            "bin/msvcp140_1.dll", "bin/msvcp140_2.dll", "bin/msvcp140_codecvt_ids.dll",
            "bin/msvcp140_atomic_wait.dll", "bin/vcruntime140_1.dll",
            "lib/clang/19/lib/windows/clang_rt.builtins-x86_64.lib",
        }
        if selected:
            entries.append((relative, header))
    offset += 46 + header[10] + header[11] + header[12]


def extract(entry):
    name, header = entry
    target = (INSTALL / name).resolve()
    if not target.is_relative_to(INSTALL.resolve()):
        raise ValueError("ZIP entry escapes installation")
    key = hashlib.sha256(name.encode()).hexdigest()[:20]
    # Local filename/extra data can differ from the directory. Include a
    # conservative amount of padding and then read the actual local lengths.
    size = 30 + header[10] + header[11] + header[8] + 4096
    record = fetch(header[-1], size, key + ".bin")
    local = struct.unpack_from("<4s5H3I2H", record)
    if local[0] != b"PK\x03\x04" or local[2] & 1:
        raise ValueError("Invalid/encrypted ZIP entry")
    start = 30 + local[-2] + local[-1]
    payload = record[start:start + header[8]]
    if header[4] == 8:
        contents = zlib.decompress(payload, -15)
    elif header[4] == 0:
        contents = payload
    else:
        raise ValueError("Unsupported ZIP compression")
    if len(contents) != header[9] or zlib.crc32(contents) != header[7]:
        raise ValueError(f"ZIP integrity check failed: {name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(contents).hexdigest()
    if not target.exists() or hashlib.file_digest(target.open("rb"), "sha256").hexdigest() != digest:
        target.write_bytes(contents)
    return {"file": name, "sha256": digest}


with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
    files = list(executor.map(extract, entries))
manifest = {
    "version": "19.1.5", "source": URL,
    "sourcePackageSha256": "3f4c2578467784756f3706848d8ebec3f0b1f6705c062b2be769f4138534115f",
    "wholeSourcePackageDownloaded": False,
    "directorySha256": hashlib.sha256(directory).hexdigest(), "files": files,
}
(INSTALL / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
print(f"Prepared Clang 19.1.5: {len(files)} x64 compiler/header files")
