#!/usr/bin/python3
# SPDX-FileCopyrightText: (C) 2026 LanBridge contributors
# SPDX-License-Identifier: MIT
"""Remove unused escaping runpaths and sign an already deployed LanBridge bundle."""

import argparse
import importlib.util
from pathlib import Path
import plistlib
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--identity", default="-")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This helper must run on macOS")
    verifier_path = Path(__file__).with_name("verify-macos-bundle.py")
    specification = importlib.util.spec_from_file_location("bundle_verifier", verifier_path)
    verifier = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(verifier)
    audit = verifier.BundleAudit(args.bundle)
    with (audit.bundle / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    executable = (audit.bundle / "Contents/MacOS" / info["CFBundleExecutable"]).resolve()
    audit.collect()
    audit.audit_dependencies(executable)
    # The validator ignores unsafe runpaths while resolving. Only delete them when every dependency
    # already resolves internally or to an Apple system library without those paths.
    blocking = [error for error in audit.errors if not error.startswith("Unsafe LC_RPATH:")]
    if blocking:
        raise RuntimeError("Cannot finalize an incomplete bundle:\n" + "\n".join(blocking))
    removed = set()
    for record in audit.bad_rpaths:
        key = (record["image"], record["rpath"])
        if key in removed:
            continue
        subprocess.run(["/usr/bin/install_name_tool", "-delete_rpath", record["rpath"],
                        str(audit.bundle / record["image"])], check=True)
        removed.add(key)
    if args.identity == "-":
        # Personal ad-hoc signatures have no Team ID. Hardened library validation would reject Qt
        # frameworks with a different signature; do not enable it for this local, non-notarized build.
        options = ["--options", "0", "--timestamp=none"]
    else:
        options = ["--options", "runtime", "--timestamp"]

    def sign(path):
        subprocess.run(["/usr/bin/codesign", "--force", "--sign", args.identity] + options + [str(path)], check=True)

    for image in sorted(audit.images, key=lambda path: len(path.parts), reverse=True):
        sign(image)
    bundles = [path for path in audit.bundle.rglob("*")
               if path.is_dir() and not path.is_symlink() and path.suffix in {".framework", ".bundle", ".xpc", ".app"}]
    for bundle in sorted(bundles, key=lambda path: len(path.parts), reverse=True):
        sign(bundle)
    sign(audit.bundle)
    subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(audit.bundle)], check=True)
    print("Finalized {} Mach-O images; removed {} unused external runpaths".format(len(audit.images), len(removed)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
