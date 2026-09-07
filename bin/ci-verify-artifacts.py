#!/usr/bin/env python3
"""Reject build artifacts whose source identity, repository or payload hashes differ."""

import hashlib
import json
import sys
from pathlib import Path


def verify(workspace, profile, origin):
    expected = json.loads((workspace / ".pio/ci-snapshot.json").read_text())
    release = workspace / "release"
    manifests = list(release.glob("firmware-*.mt.json"))
    if len(manifests) != 1:
        raise ValueError("Expected exactly one profile manifest")
    manifest = json.loads(manifests[0].read_text())
    repo = "/".join(origin.rstrip("/").removesuffix(".git").split("/")[-2:])
    checks = {
        "version": expected["long"],
        "source": expected["source"],
        "identity_sha256": expected["identity_sha256"],
        "build_epoch": expected["build_epoch"],
        "platformioTarget": profile,
        "mcu": "esp32s3",
        "repo": repo,
    }
    for key, value in checks.items():
        if manifest.get(key) != value:
            raise ValueError(
                f"Build manifest {key} does not match the requested source"
            )
    version = expected["long"]
    base = f"firmware-{profile}-{version}"
    names = {
        f"{base}.elf",
        f"{base}.bin",
        f"{base}.factory.bin",
        f"littlefs-{profile}-{version}.bin",
        "mt-esp32s3-ota.bin",
    }
    files = manifest["files"]
    if len(files) != len(names) or {item["name"] for item in files} != names:
        raise ValueError("Build manifest payload set is incomplete or duplicated")
    for item in files:
        content = (release / item["name"]).read_bytes()
        if len(content) != item["bytes"]:
            raise ValueError(f"Wrong payload size: {item['name']}")
        for algorithm in ("sha256", "md5"):
            if hashlib.new(algorithm, content).hexdigest() != item[algorithm]:
                raise ValueError(f"Wrong {algorithm}: {item['name']}")
    return {
        "status": "PASS",
        "profile": profile,
        "version": version,
        "repo": repo,
        "verified_payloads": len(files),
        "source": expected["source"],
    }


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(
            "Usage: ci-verify-artifacts.py WORKSPACE PROFILE ORIGINAL_ORIGIN_URL"
        )
    print(json.dumps(verify(Path(sys.argv[1]), sys.argv[2], sys.argv[3]), indent=2))
