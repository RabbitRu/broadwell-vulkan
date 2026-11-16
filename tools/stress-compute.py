"""Repeat a prepared fresh-ELK hardware case, preserving the first failure.

Each process creates a new WDDM device, context, VA reservation and allocations.
There are no retries: one mismatch or timeout fails the entire run.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", type=Path)
    parser.add_argument("--iterations", type=int, default=500)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("iterations must be positive")
    root = Path(__file__).resolve().parent.parent
    executable = root / "build/native/HasvkWddmProbe.exe"
    case = args.case.resolve()
    metadata = json.loads((case / "kernel.json").read_text(encoding="utf-8-sig"))
    kernel_hash = sha256(case / "kernel.bin")
    if kernel_hash != metadata["kernelSha256"]:
        raise RuntimeError("Prepared kernel checksum mismatch")
    output = args.output.resolve()
    # Do not silently overwrite evidence from an earlier attempt.
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    report = {
        "schema": 1, "startedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "executableSha256": sha256(executable), "kernelSha256": kernel_hash,
        "case": str(case), "requestedIterations": args.iterations,
        "completedIterations": 0, "retries": 0, "passed": False,
    }
    try:
        for iteration in range(args.iterations):
            target = output / f"{iteration:04d}"
            target.mkdir()
            with (target / "process.log").open("w", encoding="utf-8") as log:
                process = subprocess.run(
                    [str(executable), "elk", "0", str(case), "unused", str(target)],
                    stdout=log, stderr=subprocess.STDOUT, timeout=30, check=False,
                )
            if process.returncode:
                report["failedIteration"] = iteration
                raise RuntimeError(f"Hardware case failed, exit {process.returncode}: {target}")
            actual = json.loads((target / "report.json").read_text(encoding="utf-8-sig"))
            if not actual["overallPassed"] or actual["executableSha256"] != report["executableSha256"]:
                raise RuntimeError(f"Invalid hardware report: {target}")
            report["completedIterations"] += 1
            if (iteration + 1) % 50 == 0:
                print(f"PASS {iteration + 1}/{args.iterations}, {time.monotonic() - started:.1f}s", flush=True)
        report["passed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        report["elapsedSeconds"] = round(time.monotonic() - started, 3)
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
