#!/usr/bin/env python3
"""Copy the current Git sources into an independent Linux build checkout."""

import argparse
import importlib.util
import json
import os
import subprocess
from pathlib import Path
from urllib.parse import urlsplit, urlunsplit


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def snapshot(source, destination):
    mode_setting = subprocess.run(
        ["git", "-C", str(source), "config", "--bool", "core.filemode"],
        capture_output=True,
        text=True,
    )
    check_filemode = mode_setting.stdout.strip() != "false"
    if destination.exists() and any(destination.iterdir()):
        raise ValueError(f"Snapshot destination must be empty: {destination}")
    subprocess.run(
        [
            "git",
            "clone",
            "--quiet",
            "--no-checkout",
            "--no-local",
            "--depth=1",
            source.as_uri(),
            str(destination),
        ],
        check=True,
    )
    commit = git(source, "rev-parse", "HEAD").decode().strip()
    if git(destination, "rev-parse", "HEAD").decode().strip() != commit:
        subprocess.run(
            [
                "git",
                "-C",
                str(destination),
                "fetch",
                "--quiet",
                "--depth=1",
                "origin",
                commit,
            ],
            check=True,
        )
    subprocess.run(
        ["git", "-C", str(destination), "update-ref", "HEAD", commit], check=True
    )
    index = git(source, "ls-files", "--stage", "-z")
    blobs = set()
    for row in index.split(b"\0"):
        if not row:
            continue
        mode, oid, stage = row.split(b"\t", 1)[0].split()
        if stage != b"0":
            raise ValueError("Resolve Git conflicts before running CI")
        if mode != b"160000":
            blobs.add(oid)
    objects = subprocess.check_output(
        ["git", "-C", str(destination), "cat-file", "--batch-check"],
        input=b"\n".join(sorted(blobs)) + b"\n",
    )
    for row in objects.splitlines():
        if row.endswith(b" missing"):
            oid = row.split()[0].decode()
            subprocess.run(
                ["git", "-C", str(destination), "hash-object", "-w", "--stdin"],
                input=git(source, "cat-file", "blob", oid),
                stdout=subprocess.DEVNULL,
                check=True,
            )
    subprocess.run(["git", "-C", str(destination), "read-tree", "--empty"], check=True)
    subprocess.run(
        ["git", "-C", str(destination), "update-index", "-z", "--index-info"],
        input=index,
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(destination), "config", "core.autocrlf", "false"], check=True
    )
    subprocess.run(
        ["git", "-C", str(destination), "config", "core.filemode", "true"], check=True
    )
    entries = {}
    for row in index.split(b"\0"):
        if not row:
            continue
        metadata, name = row.split(b"\t", 1)
        mode, _, stage = metadata.split()
        if stage != b"0":
            raise ValueError("Resolve Git conflicts before running CI")
        entries[os.fsdecode(name)] = mode
    for name in git(source, "ls-files", "--others", "--exclude-standard", "-z").split(
        b"\0"
    ):
        if name:
            entries[os.fsdecode(name)] = b"100644"
    attributes = {}
    rows = subprocess.check_output(
        ["git", "-C", str(source), "check-attr", "-z", "--stdin", "eol", "text"],
        input=b"\0".join(os.fsencode(name) for name in entries) + b"\0",
    ).split(b"\0")
    for name, key, value in zip(rows[0::3], rows[1::3], rows[2::3]):
        attributes.setdefault(os.fsdecode(name), {})[key] = value
    for name, mode in entries.items():
        original, copied = source / name, destination / name
        if mode == b"160000":
            if not (original / ".git").exists():
                raise ValueError(
                    "Initialize submodules first: git submodule update --init --recursive"
                )
            snapshot(original, copied)
            continue
        if not original.exists() and not original.is_symlink():
            continue
        copied.parent.mkdir(parents=True, exist_ok=True)
        if original.is_symlink():
            copied.symlink_to(os.readlink(original))
        else:
            content = original.read_bytes()
            attrs = attributes.get(name, {})
            if b"\0" not in content and attrs.get(b"text") != b"unset":
                content = content.replace(b"\r\n", b"\n")
                if attrs.get(b"eol") == b"crlf":
                    content = content.replace(b"\n", b"\r\n")
            copied.write_bytes(content)
            executable = (
                bool(original.stat().st_mode & 0o111)
                if check_filemode
                else mode == b"100755"
            )
            copied.chmod(0o755 if executable else 0o644)

    origin = subprocess.run(
        ["git", "-C", str(source), "config", "--get", "remote.origin.url"],
        capture_output=True,
        text=True,
    )
    if origin.returncode == 1:
        git(destination, "remote", "remove", "origin")
    else:
        origin.check_returncode()
        url = origin.stdout.strip()
        if "://" in url:
            parsed = urlsplit(url)
            url = urlunsplit(
                (parsed.scheme, parsed.netloc.rsplit("@", 1)[-1], parsed.path, "", "")
            )
        git(destination, "config", "--replace-all", "remote.origin.url", url)


def identity(root):
    spec = importlib.util.spec_from_file_location(
        "ci_readprops", root / "bin/readprops.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.readProps(root / "version.properties")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if source == destination or source.is_relative_to(destination):
        parser.error("Snapshot destination must not contain the source checkout")
    before = identity(source)
    snapshot(source, destination)
    after = identity(source)
    actual = identity(destination)
    if before != after or before != actual:
        raise SystemExit(
            "Source changed during snapshot or copied source identity differs; rerun CI"
        )
    (destination / ".pio").mkdir(exist_ok=True)
    (destination / ".pio/ci-snapshot.json").write_text(
        json.dumps(actual, indent=2) + "\n"
    )
    print(json.dumps(actual, indent=2))


if __name__ == "__main__":
    main()
