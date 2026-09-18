#!/usr/bin/env python3
"""Fetch the pinned Slang compiler and verify its upstream release digest."""
import argparse
import hashlib
import pathlib
import platform
import tarfile
import urllib.request
import zipfile
VERSION = '2026.18'
PACKAGES = {
    'Linux': ('linux-x86_64-glibc-2.28.tar.gz', '8f27819f6bce2e37f3549e204b57a954d8daee67a5a5735cdc437b8bc7b87a50'),
    'Windows': ('windows-x86_64.zip', '6ffa4827b519fd0a85b38407049d87ab0c1f045fe2289cb1e6831f965169f8a1'),
}
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('.cache/slang'))
    args = parser.parse_args()
    if platform.machine().lower() not in ('x86_64', 'amd64'):
        raise SystemExit('Pinned Slang packages currently support x86-64 hosts; supply SLANGC_EXECUTABLE for another host.')
    suffix, digest = PACKAGES[platform.system()]
    name = f'slang-{VERSION}-{suffix}'
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output / name
    if not archive.exists():
        url = f'https://github.com/shader-slang/slang/releases/download/v{VERSION}/{name}'
        temporary = archive.with_suffix('.download')
        urllib.request.urlretrieve(url, temporary)
        temporary.replace(archive)
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != digest:
        raise SystemExit(f'Slang checksum mismatch for {archive}; expected {digest}, received {actual}')
    if suffix.endswith('.zip'):
        with zipfile.ZipFile(archive) as package:
            for item in package.infolist():
                target = (args.output / item.filename).resolve()
                if not target.is_relative_to(args.output.resolve()):
                    raise SystemExit('Unsafe archive path')
            package.extractall(args.output)
    else:
        with tarfile.open(archive) as package:
            package.extractall(args.output, filter='data')
    executable = args.output / 'bin' / ('slangc.exe' if platform.system() == 'Windows' else 'slangc')
    if not executable.is_file():
        raise SystemExit(f'Compiler missing from package: {executable}')
    print(executable.resolve())
if __name__ == '__main__':
    main()
