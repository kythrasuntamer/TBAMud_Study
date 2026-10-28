#!/usr/bin/env python3
"""Download pinned MUD source snapshots and inventory their mobile/object data.

Does not install or convert files. Downloads are quarantined. Source archives
may contain third-party copyrighted areas; review rights before redistribution.

Every catalog entry must pin "ref" (a 40-hex commit). If it also pins
"sha256" (of the GitHub tarball for that commit) the download must match it;
an entry without sha256 is refused unless --allow-unhashed is given, and the
hash it produced is then recorded so it can be pinned.
"""
import argparse
import fnmatch
import hashlib
import shutil
import io
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import time
import urllib.request
import sys

MAX_DOWNLOAD = 150_000_000
MAX_FILE = 8_000_000
DEADLINE = 300            # seconds for one whole download, not per socket read
CHUNK = 1 << 16
REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
REF = re.compile(r"[0-9a-f]{40}")
SHA256 = re.compile(r"[0-9a-f]{64}")


def check_entry(entry, allow_unhashed=False):
    """Reject a catalog entry that cannot be fetched reproducibly."""
    repo = entry.get("repo", "")
    if not REPO.fullmatch(repo) or ".." in repo:
        raise ValueError(f"invalid repo {repo!r}")
    if not REF.fullmatch(entry.get("ref") or ""):
        raise ValueError(f"{repo}: catalog entry must pin a 40-hex commit 'ref'")
    sha = entry.get("sha256")
    if sha is None and not allow_unhashed:
        raise ValueError(f"{repo}: no pinned sha256 (use --allow-unhashed to record one)")
    if sha is not None and not SHA256.fullmatch(sha):
        raise ValueError(f"{repo}: malformed sha256 {sha!r}")


def safe_rel(name):
    """Archive member name without its "owner-repo-<sha>/" top directory."""
    rel = name.split("/", 1)[-1]
    parts = PurePosixPath(rel).parts
    if not rel or rel.startswith("/") or ".." in parts:
        raise ValueError(f"Unsafe archive path: {name}")
    return rel


def scan_archive(blob, entry):
    """All matching files, or an exception: nothing is returned partially."""
    manifest, top = [], None
    patterns = [entry.get(key) for key in ("mob_glob", "obj_glob", "area_glob") if entry.get(key)]
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
        for member in archive:
            top = top or member.name.split("/", 1)[0]
            rel = safe_rel(member.name)
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
    return manifest, top


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": "tbamud-source-auditor"})
    deadline = time.monotonic() + DEADLINE
    chunks, total = [], 0
    with urllib.request.urlopen(req, timeout=45) as response:
        final = response.geturl()
        if not final.startswith("https://"):
            raise ValueError(f"refusing non-HTTPS redirect to {final}")
        while True:
            if time.monotonic() > deadline:
                raise ValueError(f"download exceeded {DEADLINE}s")
            chunk = response.read(CHUNK)
            if not chunk:
                break
            total += len(chunk)
            if total > MAX_DOWNLOAD:
                raise ValueError("Archive exceeds limit")
            chunks.append(chunk)
    return b"".join(chunks), final


def stage_files(folder, extracted):
    """Write (relative path, bytes) pairs into folder all at once: into a
    sibling <folder>.tmp that is renamed only when every file is in, and
    removed on any failure. An existing folder or staging folder is refused."""
    staging = folder.with_name(folder.name + ".tmp")
    for path in (folder, staging):
        if path.exists():
            raise ValueError(f"{path} already exists; refusing to mix snapshots")
    staging.mkdir()
    try:
        for rel, data in extracted:
            if not rel or rel.startswith("/") or ".." in PurePosixPath(rel).parts:
                raise ValueError(f"unsafe relative path {rel!r} for {folder.name}")
            dest = staging / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        staging.rename(folder)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--catalog", type=Path, default=Path("imports/source-catalog.json"))
    p.add_argument("--destination", type=Path, required=True)
    p.add_argument("--apply", action="store_true", help="Download into a new quarantine folder")
    p.add_argument("--allow-unhashed", action="store_true",
                   help="Fetch entries without a pinned sha256 and record the hash")
    args = p.parse_args()
    catalog = json.loads(args.catalog.read_text())
    for entry in catalog["sources"]:
        print(entry.get("repo"), entry.get("format"), entry.get("ref"), "AUDIT ONLY")
    if not args.apply:
        print("Dry run; use --apply to fetch source tarballs (no game installation).")
        return 0
    if args.destination.exists():
        raise ValueError("Destination must not exist")
    args.destination.mkdir(parents=True)
    report = []
    failures = 0
    for entry in catalog["sources"]:
        repo = entry.get("repo", "?")
        try:
            check_entry(entry, args.allow_unhashed)
            raw, final = download(f"https://api.github.com/repos/{repo}/tarball/{entry['ref']}")
            digest = hashlib.sha256(raw).hexdigest()
            if entry.get("sha256") and digest != entry["sha256"]:
                raise ValueError(f"tarball sha256 {digest} != pinned {entry['sha256']}")
            extracted, top = scan_archive(raw, entry)
            if not extracted:
                raise ValueError("No matching source files; catalog glob or source layout changed")
            folder = args.destination / repo.replace("/", "__")
            rows = [{"repo": repo, "ref": entry["ref"], "snapshot": top,
                     "tarball_sha256": digest,
                     "tarball_sha256_pinned": bool(entry.get("sha256")),
                     "path": rel, "sha256": hashlib.sha256(data).hexdigest(),
                     "bytes": len(data), "snapshot_url": final,
                     "status": "QUARANTINED_NOT_CONVERTED"} for rel, data in extracted]
            stage_files(folder, extracted)
            report.extend(rows)
            print(f"{repo}@{entry['ref'][:12]}: {len(extracted)} eligible-format files inventoried")
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
