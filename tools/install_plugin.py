#!/usr/bin/env python3
"""Install the input plugin with a restorable backup of the selected Xcode's plugin."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from xrctl import ROOT, developer_dir


def digest(path):
    h = hashlib.sha256()
    for file in sorted(path.rglob("*")):
        if file.is_file():
            h.update(str(file.relative_to(path)).encode())
            h.update(file.read_bytes())
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "uninstall"])
    parser.add_argument("--bundle-dir", type=Path, default=ROOT / "build/modern/bundle")
    args = parser.parse_args()
    dev = developer_dir().resolve()
    target = dev / "Platforms/XROS.platform/Library/Developer/CoreSimulator/Profiles/UserInterface/XROS.simdeviceui"
    backup_root = ROOT / "build/plugin-backup" / hashlib.sha256(str(dev).encode()).hexdigest()[:12]
    backup = backup_root / "XROS.simdeviceui"
    record = backup_root / "installation.json"
    # Do not begin a replacement unless both the bundle and its parent are
    # writable. Xcode is often root-owned; callers can explicitly use sudo.
    import os
    if not os.access(target.parent, os.W_OK) or (target.exists() and not os.access(target, os.W_OK)):
        raise RuntimeError("Xcode's plugin is not writable. Run this installer with administrator privileges; no files changed.")
    if args.action == "install":
        source = args.bundle_dir.resolve() / "XRGyroControls.simdeviceui"
        if not (source / "Contents/MacOS/XRGyroControls").is_file():
            raise RuntimeError("Built input plugin missing. Run ./build.sh first.")
        if not target.is_dir():
            raise RuntimeError(f"Xcode's visionOS UI plugin is missing: {target}")
        if record.exists():
            previous = json.loads(record.read_text())
            if digest(target) != previous["installed_sha256"]:
                raise RuntimeError("Xcode's plugin changed since installation; refusing to overwrite it. Restore or update the backup explicitly.")
        else:
            backup_root.mkdir(parents=True, exist_ok=True)
            if backup.exists():
                if digest(backup) != digest(target):
                    raise RuntimeError("Existing backup differs from the current Xcode plugin; inspect it before reinstalling.")
            else:
                shutil.copytree(target, backup, symlinks=True)
            record.write_text(json.dumps({"target": str(target), "original_sha256": digest(backup),
                                          "installed_sha256": digest(target)}, indent=2) + "\n")
        try:
            shutil.rmtree(target)
            shutil.copytree(source, target, symlinks=True)
        except Exception:
            if target.exists():
                shutil.rmtree(target)
            shutil.copytree(backup, target, symlinks=True)
            raise
        previous = json.loads(record.read_text())
        previous["installed_sha256"] = digest(target)
        record.write_text(json.dumps(previous, indent=2) + "\n")
        print(f"Installed input plugin: {target}\nOriginal backup: {backup}")
        print("Quit and reopen Device Hub/Simulator before testing. No device profiles or security settings were changed.")
    else:
        if not record.is_file() or not backup.is_dir():
            raise RuntimeError("No installation backup for this Xcode. Nothing changed.")
        previous = json.loads(record.read_text())
        if digest(backup) != previous["original_sha256"]:
            raise RuntimeError("Backup has changed; refusing to restore it.")
        if target.exists() and digest(target) != previous["installed_sha256"]:
            raise RuntimeError("Xcode's plugin changed since installation; refusing to overwrite it.")
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(backup, target, symlinks=True)
        record.unlink()
        # Keep the original backup as an audit artifact; subsequent installation
        # can use a fresh build directory after the backup is reviewed.
        print(f"Restored original plugin: {target}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError) as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
