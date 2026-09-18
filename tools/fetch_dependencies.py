#!/usr/bin/env python3
"""Download pinned source archives once; CMake reuses them without network access."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    directory = ROOT / ".cache" / "downloads"
    directory.mkdir(parents=True, exist_ok=True)

    def fetch(item):
        name, dep = item
        path = directory / f"{name}-{dep['commit']}.tar.gz"
        if not path.exists():
            if args.verify_only:
                raise RuntimeError(f"Missing archive: {path}")
            with urllib.request.urlopen(dep["url"], timeout=180) as response:
                content = response.read()
            if hashlib.sha256(content).hexdigest() != dep["sha256"]:
                raise RuntimeError(f"Checksum mismatch: {name}")
            temporary = path.with_suffix(".download")
            temporary.write_bytes(content)
            temporary.replace(path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != dep["sha256"]:
            raise RuntimeError(f"Checksum mismatch: {name}")
        print(f"Verified {name}: {dep['commit']}", flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(fetch, lock["dependencies"].items()))

if __name__ == "__main__":
    main()
