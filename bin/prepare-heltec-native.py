#!/usr/bin/env python3
"""Install the audited host-only adapter in a disposable source snapshot."""

import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from platformio.package.manager.library import LibraryPackageManager

FIXTURE_REVISION = "7239fe886a30fa13cd35946fa5ae1a46a2807eeb"
ROOT = Path(__file__).resolve().parents[1]


def main():
    if not (ROOT / ".pio/ci-snapshot.json").is_file():
        raise SystemExit(
            "Run bin/test-native-docker.sh to prepare an isolated source snapshot"
        )
    if not (ROOT / ".git").is_dir():
        raise SystemExit("The native fixture requires a standalone source snapshot")
    if (ROOT / "src/platform/portduino").exists() or (
        ROOT / "variants/native"
    ).exists():
        raise SystemExit(
            "Refusing to replace an existing native configuration or source"
        )
    ini = ROOT / "platformio.ini"
    original = ini.read_text()
    needle = "\tvariants/esp32s3/heltec_v4/platformio.ini"
    if original.count(needle) != 1:
        raise SystemExit(
            "Heltec PlatformIO configuration changed; audit the fixture setup"
        )
    for script in ("heltec_release_policy_test.py", "heltec_audit_regression.py"):
        subprocess.run(
            ["python3", str(ROOT / "test/host" / script)], check=True, cwd=ROOT
        )
    (ROOT / ".pio").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="native-fixture-", dir=ROOT / ".pio"
    ) as directory:
        fixture = Path(directory)

        def git(*args):
            return subprocess.check_output(
                ["git", "-C", str(fixture), *args], text=True
            ).strip()

        git("init", "-q")
        git("remote", "add", "origin", "https://github.com/meshtastic/firmware.git")
        git("fetch", "--filter=blob:none", "--depth=1", "origin", FIXTURE_REVISION)
        git("sparse-checkout", "set", "variants/native", "src/platform/portduino")
        git("checkout", "--detach", "FETCH_HEAD")
        if git("rev-parse", "HEAD") != FIXTURE_REVISION:
            raise SystemExit("Native fixture revision mismatch")
        destination = ROOT / "variants/native"
        destination.mkdir()
        shutil.copy2(fixture / "variants/native/portduino.ini", destination)
        shutil.copytree(
            fixture / "variants/native/portduino", destination / "portduino"
        )
        shutil.copytree(
            fixture / "src/platform/portduino", ROOT / "src/platform/portduino"
        )
    native = ROOT / "variants/native/portduino/platformio.ini"
    sections = re.split(r"(?=^\[)", native.read_text(), flags=re.M)
    keep = {"native_base", "env:native", "env:coverage"}
    selected = [
        s for s in sections if s.startswith("[") and s[1 : s.index("]")] in keep
    ]
    if len(selected) != len(keep):
        raise SystemExit("Native fixture sections changed")
    native_config = "".join(selected)
    dependency = "${portduino_base.lib_deps}"
    if native_config.count(dependency) != 1:
        raise SystemExit("Native fixture dependency inheritance changed")
    native_config = native_config.replace(
        dependency, dependency + "\n  throwtheswitch/Unity@2.6.1"
    )
    native.write_text(
        native_config + "\n" + (ROOT / "test/host/heltec_storage_audit.ini").read_text()
    )
    ini.write_text(
        original.replace(
            needle,
            needle
            + "\n\tvariants/native/portduino.ini\n\tvariants/native/portduino/platformio.ini",
        )
    )
    for environment in ("coverage", "coverage-storage"):
        manager = LibraryPackageManager(str(ROOT / ".pio/libdeps" / environment))
        for package in manager.get_installed():
            if (
                package.metadata.name == "Unity"
                and str(package.metadata.version) != "2.6.1"
            ):
                manager.uninstall(package)
    report = ROOT / "audit-results"
    report.mkdir(exist_ok=True)
    (report / "fixture.json").write_text(
        json.dumps({"revision": FIXTURE_REVISION}, indent=2) + "\n"
    )


if __name__ == "__main__":
    main()
