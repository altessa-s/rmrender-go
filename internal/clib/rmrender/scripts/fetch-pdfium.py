#!/usr/bin/env python3
# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Fetch the pinned, checksum-verified PDFium test SDK. Never run by CMake."""
import argparse
import hashlib
import io
import platform
import subprocess
from pathlib import Path
import tarfile
import urllib.request

REVISION = "8125"
HASHES = {
    "win-x64": "9a964261760cff52570aabf038ab346d3a5034dd8b30d0d5fe8861235d356b8b",
    "mac-arm64": "4939bd935ac83ddc09a3a4f038bc43a19cea1660316a3667f26cb07a070e139d",
    "mac-x64": "8a5193535b80c7215d8932f6647ae5097465b6dba2811602c21e120c2af112b0",
    "linux-x64": "e1c3ce6e270cb5e0c20816b379ad9f2c1309972697c505f508de92bf5e7d6659",
    "linux-arm64": "8b8818f2ec57d1b213d952ca6d7f0ffeca9d0110487d0e20ad83ba918bc92efa",
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--platform", choices=HASHES, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if args.output.exists() and any(args.output.iterdir()):
    parser.error("output directory must be empty")
url = ("https://github.com/bblanchon/pdfium-binaries/releases/download/"
       f"chromium/{REVISION}/pdfium-{args.platform}.tgz")
with urllib.request.urlopen(url, timeout=120) as response:
    data = response.read(128 * 1024 * 1024 + 1)
if hashlib.sha256(data).hexdigest() != HASHES[args.platform]:
    raise SystemExit("PDFium checksum mismatch")
args.output.mkdir(parents=True, exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
    archive.extractall(args.output, filter="data")
if args.platform.startswith("mac-") and platform.system() == "Darwin":
    library = args.output / "lib" / "libpdfium.dylib"
    # The upstream archive uses ./libpdfium.dylib. Use a relocatable install
    # name and restore an ad-hoc signature after editing the Mach-O header.
    subprocess.run(["install_name_tool", "-id", "@rpath/libpdfium.dylib", str(library)], check=True)
    subprocess.run(["codesign", "--force", "--sign", "-", str(library)], check=True)
print(f"PDFium chromium/{REVISION}: {args.output}")
