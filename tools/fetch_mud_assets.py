#!/usr/bin/env python3
"""Download selected MUD source snapshots and inventory all mobile/object data.

Does not install or convert files. Downloads are quarantined. Source archives
may contain third-party copyrighted areas; review rights before redistribution.
"""
import argparse
import fnmatch
import hashlib
import io
import json
from pathlib import Path
import tarfile
import urllib.request
import sys

MAX_DOWNLOAD = 150_000_000
MAX_FILE = 8_000_000

def scan_archive(blob, entry):
    manifest = []
    patterns = [entry.get(key) for key in ("mob_glob", "obj_glob", "area_glob") if entry.get(key)]
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
        for member in archive:
            rel = member.name.split("/", 1)[-1]
            if not member.isfile() or not any(fnmatch.fnmatchcase(rel, p) for p in patterns):
                continue
            if member.size > MAX_FILE or member.size < 0:
                raise ValueError(f"Source too large: {rel}")
            stream = archive.extractfile(member)
            if stream is None:
                raise ValueError(f"Missing archive data: {rel}")
            data = stream.read(MAX_FILE + 1)
            if len(data) != member.size:
                raise ValueError(f"Invalid source size: {rel}")
            manifest.append((rel, data))
    return manifest

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=Path("imports/source-catalog.json"))
    p.add_argument("--destination", type=Path, required=True)
    p.add_argument("--apply", action="store_true", help="Download into a new quarantine folder")
    args = p.parse_args()
    catalog = json.loads(args.catalog.read_text())
    for entry in catalog["sources"]:
        print(entry["repo"], entry["format"], "AUDIT ONLY")
    if not args.apply:
        print("Dry run; use --apply to fetch source tarballs (no game installation).")
        return 0
    if args.destination.exists():
        raise ValueError("Destination must not exist")
    args.destination.mkdir(parents=True)
    report = []
    failures = 0
    for entry in catalog["sources"]:
        repo = entry["repo"]
        url = f"https://api.github.com/repos/{repo}/tarball/HEAD"
        req = urllib.request.Request(url, headers={"User-Agent": "tbamud-source-auditor"})
        try:
            with urllib.request.urlopen(req, timeout=45) as response:
                raw = response.read(MAX_DOWNLOAD + 1)
                final = response.geturl()
            if len(raw) > MAX_DOWNLOAD:
                raise ValueError("Archive exceeds limit")
            extracted = scan_archive(raw, entry)
            if not extracted:
                raise ValueError("No matching source files; catalog glob or source layout changed")
            name = repo.replace("/", "__")
            folder = args.destination / name
            folder.mkdir()
            for rel, data in extracted:
                if Path(rel).is_absolute() or ".." in Path(rel).parts:
                    raise ValueError(f"Unsafe archive path: {rel}")
                dest = folder / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                report.append({"repo": repo, "path": rel, "sha256": hashlib.sha256(data).hexdigest(),
                               "bytes": len(data), "snapshot_url": final,
                               "status": "QUARANTINED_NOT_CONVERTED"})
            print(f"{repo}: {len(extracted)} eligible-format files inventoried")
        except Exception as exc:
            report.append({"repo": repo, "error": str(exc), "status": "FAILED"})
            failures += 1
            print(f"FAILED {repo}: {exc}")
    (args.destination / "provenance.json").write_text(json.dumps(report, indent=2) + "\n")
    if failures:
        print(f"{failures} source downloads failed; quarantine is incomplete", file=sys.stderr)
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
