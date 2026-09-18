#!/usr/bin/env python3
"""Linux clean-checkout acceptance in a network namespace with no external network.

Uses committed HEAD, prefetched checksum-verified archives and local system tools.
Does not download dependencies, install packages or change the source checkout.
"""
import argparse
import datetime
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=4)
    args = parser.parse_args()
    if sys.platform != "linux" or not shutil.which("unshare"):
        parser.error("This check requires Linux unshare with user/network namespace support")
    if not 1 <= args.parallel <= 64:
        parser.error("--parallel must be between 1 and 64")
    root, output = Path(__file__).resolve().parents[1], args.output.resolve()
    if output.exists():
        parser.error("Use a new output directory to preserve previous evidence")
    subprocess.run([sys.executable, root / "tools/fetch_dependencies.py", "--verify-only"], check=True)
    if not (root / ".cache/slang/bin/slangc").is_file():
        parser.error("Prefetch Slang before starting the offline check")
    # Verify capability before making a potentially expensive copy.
    subprocess.run(["unshare", "--user", "--map-root-user", "--net", "true"], check=True)
    output.mkdir(parents=True)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    archive, checkout = output / "source.tar", output / "checkout"
    with archive.open("wb") as stream:
        subprocess.run(["git", "archive", "--format=tar", revision], cwd=root, stdout=stream, check=True)
    with tarfile.open(archive) as content:
        content.extractall(checkout, filter="data")
    archive.unlink()
    shutil.copytree(root / ".cache/downloads", checkout / ".cache/downloads")
    shutil.copytree(root / ".cache/slang", checkout / ".cache/slang", symlinks=True)
    script = output / "offline_driver.py"
    script.write_text('''import json, pathlib, socket, subprocess, sys
root = pathlib.Path(sys.argv[1])
# No external interfaces exist in this newly created network namespace.
with socket.socket() as connection:
    connection.settimeout(2)
    try:
        connection.connect(("1.1.1.1", 443))
    except OSError as error:
        print("External network unavailable:", error, flush=True)
    else:
        raise RuntimeError("Offline check unexpectedly has external connectivity")
subprocess.run([sys.executable, "tools/fetch_dependencies.py", "--verify-only"], cwd=root, check=True)
subprocess.run(["cmake", "--preset", "linux-debug"], cwd=root, check=True)
subprocess.run(["cmake", "--build", "--preset", "linux-debug", "--parallel", sys.argv[2]], cwd=root, check=True)
subprocess.run(["ctest", "--preset", "linux-debug", "-LE", "gpu", "--timeout", "120"], cwd=root, check=True)
''', encoding="utf-8")
    started = time.perf_counter()
    with (output / "build.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(["unshare", "--user", "--map-root-user", "--net",
            sys.executable, script, checkout, str(args.parallel)], stdout=log, stderr=subprocess.STDOUT)
    report = {"format": "faset.offline-build", "version": 1, "revision": revision,
              "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "seconds": time.perf_counter() - started, "returncode": result.returncode,
              "network": "fresh user/network namespace; external connection must fail",
              "source": "git archive of committed HEAD; no existing build directory",
              "inputs": "prefetched archives + Slang; system compiler, SDK and development libraries",
              "tests": "full native build, CPU CTests; GPU/window execution verified separately"}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
