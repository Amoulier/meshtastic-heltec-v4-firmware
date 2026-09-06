#!/usr/bin/env python3
"""Exercise Git source identity and the actual PlatformIO version/manifest hooks."""

import base64
import configparser
import csv
import hashlib
import json
import os
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bin"))
from readprops import readProps  # noqa: E402


class BuildFile:
    def __init__(self, path):
        self.path = Path(path)

    def exists(self):
        return self.path.exists()

    def get_abspath(self):
        return str(self.path)

    def get_content_hash(self):
        return hashlib.md5(self.path.read_bytes()).hexdigest()

    def get_size(self):
        return self.path.stat().st_size


class BuildEnv(dict):
    def __init__(self, root, target="heltec-v4-standard"):
        super().__init__(
            PROJECT_DIR=str(root), PIOENV=target, ENV={}, BOARD_MCU="esp32s3"
        )
        self.config = configparser.RawConfigParser()
        self.config.add_section("env:" + target)
        self.config.set("env:" + target, "custom_sdkconfig", "CONFIG_SPIRAM=y")
        self.config.set("env:" + target, "custom_meshtastic_hw_model_slug", "HELTEC_V4")
        self.config.set("env:" + target, "custom_meshtastic_partition_scheme", "16MB")
        rows = csv.reader((ROOT / "default_16MB.csv").read_text().splitlines())
        parts = [
            dict(
                zip(
                    ("name", "type", "subtype", "offset", "size", "flags"),
                    map(str.strip, row),
                )
            )
            for row in rows
            if row and not row[0].startswith("#")
        ]
        self["custom_mtjson_part"] = json.dumps(parts)
        self.build_dir = root / ".pio" / target
        self.build_dir.mkdir(parents=True, exist_ok=True)

    def PioPlatform(self):
        return SimpleNamespace(name="espressif32")

    def Replace(self, **values):
        self.update(values)

    def Append(self, **values):
        for key, value in values.items():
            self.setdefault(key, []).extend(value)

    def GetProjectConfig(self):
        return self.config

    def GetProjectOption(self, name, default=None):
        return self.config.get("env:" + self["PIOENV"], name, fallback=default)

    def GetLibBuilders(self):
        return []

    def BoardConfig(self):
        return {"build.mcu": "esp32s3", "platform": "espressif32"}

    def subst(self, value):
        for key, replacement in (
            ("$BUILD_DIR", str(self.build_dir)),
            ("${PROGNAME}", self.get("PROGNAME", "")),
            ("${ESP32_FS_IMAGE_NAME}", self.get("ESP32_FS_IMAGE_NAME", "")),
            ("$SIZETOOL", ""),
        ):
            value = value.replace(key, replacement)
        return value

    def File(self, path):
        return BuildFile(path)

    def DataToBin(self, *args):
        return "littlefs-target"

    def AddCustomTarget(self, **kwargs):
        return None

    def Default(self, *args):
        return None


class VersionIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="heltec-version-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("GIT_")
        }
        env.pop("SOURCE_DATE_EPOCH", None)
        env.update(
            GIT_AUTHOR_DATE="2026-09-01T00:00:00Z",
            GIT_COMMITTER_DATE="2026-09-01T00:00:00Z",
        )
        self.env_patch = patch.dict(os.environ, env, clear=True)
        self.env_patch.start()
        self.addCleanup(self.env_patch.stop)
        self.git("init", "-q")
        self.git("config", "user.name", "Version Test")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "core.filemode", "false")
        self.git(
            "remote",
            "add",
            "origin",
            "https://github.com/Amoulier/meshtastic-heltec-v4-firmware.git",
        )
        self.write(
            "version.properties",
            "[VERSION]\nmajor=2\nminor=8\nbuild=0\nfork_revision=9\n",
        )
        self.write(".gitattributes", "* text=auto eol=lf\n")
        self.write(".gitignore", ".pio/\nrelease/\n__pycache__/\n")
        self.write("main.cpp", "int value = 1;\n")
        self.write("userPrefs.jsonc", "{}\n")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def write(self, path, content):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content.encode() if isinstance(content, str) else content)

    def git(self, *args):
        return (
            subprocess.check_output(
                ["git", "-C", str(self.root), *args], stderr=subprocess.PIPE
            )
            .decode()
            .strip()
        )

    def version(self):
        return readProps(self.root / "version.properties")

    def test_clean_checkout_is_stable_and_fits_both_wire_fields(self):
        version = self.version()
        self.assertEqual(version, self.version())
        self.assertRegex(version["long"], r"^2\.8\.0-h9g[a-z2-7]{8}$")
        self.assertEqual(version["short"], "2.8.0-h9")
        self.assertEqual(version["upstream"], "2.8.0")
        self.assertEqual(version["source"]["commit"], self.git("rev-parse", "HEAD"))
        self.assertEqual(
            version["build_epoch"], int(self.git("show", "-s", "--format=%ct"))
        )
        for relative in (
            "src/mesh/generated/meshtastic/mesh.pb.h",
            "src/mesh/generated/meshtastic/mqtt.pb.h",
        ):
            capacities = re.findall(
                r"char firmware_version\[(\d+)\]", (ROOT / relative).read_text()
            )
            self.assertTrue(capacities)
            self.assertLess(len(version["long"].encode()), min(map(int, capacities)))

    def test_distinct_dirty_contents_have_distinct_versions_and_restore_identity(self):
        clean = self.version()
        self.write("main.cpp", "int value = 2;\n")
        first = self.version()
        self.write("main.cpp", "int value = 3;\n")
        second = self.version()
        self.assertRegex(first["long"], r"-h9d[a-z2-7]{8}$")
        self.assertEqual(first["source"]["commit"], clean["source"]["commit"])
        self.assertNotEqual(first["long"], second["long"])
        self.write("main.cpp", "int value = 1;\n")
        self.assertEqual(clean, self.version())

    def test_untracked_source_and_staging_preserve_same_content_identity(self):
        clean = self.version()
        self.write("src/new file.cpp", "int added = 1;\n")
        untracked = self.version()
        self.assertNotEqual(clean["long"], untracked["long"])
        self.git("add", "src/new file.cpp")
        self.assertEqual(untracked, self.version())
        self.write("src/new file.cpp", "int added = 2;\n")
        self.assertNotEqual(untracked["long"], self.version()["long"])

    def test_deleted_source_and_staging_preserve_same_identity(self):
        clean = self.version()
        (self.root / "main.cpp").unlink()
        deleted = self.version()
        self.assertNotEqual(clean["long"], deleted["long"])
        self.git("add", "-u")
        self.assertEqual(deleted, self.version())

    def test_build_outputs_clock_and_ci_job_number_do_not_change_version(self):
        original = self.version()
        self.write(".pio/verification/summary.json", "generated")
        self.write("release/firmware.bin", b"binary\0")
        os.utime(self.root / "main.cpp", (1, 1))
        with patch.dict(
            os.environ,
            GITHUB_RUN_NUMBER="99999",
            BUILD_LOCATION="ci",
            TZ="Pacific/Honolulu",
        ):
            self.assertEqual(original, self.version())

    def test_crlf_checkout_matches_lf_checkout(self):
        original = self.version()
        self.write("main.cpp", "int value = 1;\r\n")
        self.assertEqual(original, self.version())

    def test_binary_bytes_are_not_text_normalized(self):
        self.write("asset.bin", b"\0\r\n")
        original = self.version()
        self.write("asset.bin", b"\0\n")
        self.assertNotEqual(original["long"], self.version()["long"])

    def test_source_date_epoch_is_explicit_and_part_of_identity(self):
        original = self.version()
        with patch.dict(os.environ, SOURCE_DATE_EPOCH="1234567890"):
            changed = self.version()
            self.assertEqual(1234567890, changed["build_epoch"])
            self.assertEqual(original["source"], changed["source"])
            self.assertNotEqual(original["long"], changed["long"])
            self.assertEqual(changed, self.version())
        for invalid in ("-1", "4294967296", "invalid"):
            with patch.dict(os.environ, SOURCE_DATE_EPOCH=invalid), self.assertRaises(
                ValueError
            ):
                self.version()

    def test_unidentified_or_oversize_version_fails_instead_of_reusing_upstream(self):
        self.write(
            "version.properties",
            "[VERSION]\nmajor=2000\nminor=8\nbuild=0\nfork_revision=9\n",
        )
        with self.assertRaisesRegex(ValueError, "17-byte"):
            self.version()
        with tempfile.TemporaryDirectory(prefix="heltec-no-git-") as empty:
            prefs = Path(empty) / "version.properties"
            shutil.copyfile(self.root / "version.properties", prefs)
            with self.assertRaisesRegex(RuntimeError, "Git checkout"):
                readProps(prefs)

    def test_revision_and_upstream_growth_fit_api_and_filenames(self):
        for revision, upstream_patch, suffix_length in (
            (9, 0, 8),
            (10, 0, 7),
            (99, 0, 7),
            (100, 0, 6),
            (9, 10, 7),
            (10, 10, 6),
            (99, 10, 6),
        ):
            with self.subTest(revision=revision, upstream_patch=upstream_patch):
                self.write(
                    "version.properties",
                    f"[VERSION]\nmajor=2\nminor=8\nbuild={upstream_patch}\nfork_revision={revision}\n",
                )
                version = self.version()
                prefix = f"2.8.{upstream_patch}-h{revision}"
                self.assertEqual(version["short"], prefix)
                self.assertEqual(version["fork_revision"], revision)
                self.assertEqual(version["upstream"], f"2.8.{upstream_patch}")
                self.assertEqual(len(version["long"].encode("ascii")), 17)
                self.assertRegex(
                    version["long"],
                    rf"^{re.escape(prefix)}[dg][a-z2-7]{{{suffix_length}}}$",
                )
                encoded_digest = (
                    base64.b32encode(bytes.fromhex(version["identity_sha256"]))
                    .decode("ascii")
                    .lower()
                )
                self.assertTrue(
                    version["long"].endswith(encoded_digest[:suffix_length])
                )
                self.assertEqual(len(version["source"]["sha256"]), 64)
                self.assertEqual(len(version["identity_sha256"]), 64)
                self.assertEqual(version, self.version())
                for target in ("heltec-v4-standard", "heltec-v4-solar-router"):
                    for extension in (".bin", ".factory.bin", ".mt.json"):
                        filename = f"firmware-{target}-{version['long']}{extension}"
                        self.assertEqual(
                            len(filename),
                            len(f"firmware-{target}-") + 17 + len(extension),
                        )
                        self.assertRegex(filename, r"^[0-9A-Za-z._-]+$")

    def test_api_limit_preserves_at_least_thirty_fingerprint_bits(self):
        self.write(
            "version.properties",
            "[VERSION]\nmajor=2\nminor=8\nbuild=10\nfork_revision=100\n",
        )
        with self.assertRaisesRegex(ValueError, "fewer than 6 fingerprint characters"):
            self.version()

    def test_same_tree_in_different_directory_has_same_version(self):
        with tempfile.TemporaryDirectory(prefix="heltec-version-copy-") as directory:
            destination = Path(directory) / "repo"
            shutil.copytree(self.root, destination)
            self.assertEqual(
                self.version(), readProps(destination / "version.properties")
            )

    def test_submodule_pin_and_local_modifications_are_part_of_identity(self):
        nested = self.root / "dependency"
        self.write("dependency/api.h", "int api();\n")
        for args in (
            ("init", "-q"),
            ("config", "user.name", "Version Test"),
            ("config", "user.email", "test@example.invalid"),
            ("add", "."),
            ("commit", "-qm", "dependency"),
        ):
            subprocess.run(
                ["git", "-C", str(nested), *args], check=True, capture_output=True
            )
        self.git("add", "dependency")
        self.git("commit", "-qm", "add dependency")
        clean = self.version()
        self.assertFalse(clean["source"]["dirty"])
        self.write("dependency/api.h", "int api(int value);\n")
        dirty = self.version()
        self.assertTrue(dirty["source"]["dirty"])
        self.assertNotEqual(clean["long"], dirty["long"])
        self.write("dependency/api.h", "int api();\n")
        self.assertEqual(clean, self.version())

    @unittest.skipUnless(
        sys.platform.startswith("linux") and shutil.which("jq"),
        "POSIX updater requires Linux and jq",
    )
    def test_actual_updater_accepts_new_manifest_without_device_access(self):
        stub = self.root / ".pio" / "esptool-stub"
        stub.parent.mkdir(parents=True)
        stub.write_text(
            '#!/bin/sh\nset -e\nfor arg do\ncase "$arg" in\n'
            "version) echo 'esptool.py v4.5.1'; exit 0;;\n"
            "--help) echo 'write_flash'; exit 0;;\n"
            "chip_id) echo 'ESP32-S3'; exit 0;;\n"
            "flash_id) echo 'Detected flash size: 16MB'; exit 0;;\n"
            'write_flash) printf \'%s\\n\' "$*" > "$STUB_CALL"; exit 0;;\n'
            "esac\ndone\nexit 1\n"
        )
        stub.chmod(0o755)
        for dirty in (False, True):
            if dirty:
                self.write("main.cpp", "int value = 2;\n")
            for target in ("heltec-v4-standard", "heltec-v4-solar-router"):
                with self.subTest(dirty=dirty, target=target):
                    env, module = self.hooks(target)
                    image = env.build_dir / (env["PROGNAME"] + ".bin")
                    image.write_bytes(b"fake-image\0")
                    module["manifest_gather"](None, None, env)
                    calls = env.build_dir / "calls.txt"
                    process_env = dict(os.environ, STUB_CALL=str(calls))
                    result = subprocess.run(
                        [
                            "bash",
                            str(ROOT / "bin/device-update.sh"),
                            "-p",
                            "/dev/null",
                            "-P",
                            str(stub),
                            "-f",
                            str(image),
                        ],
                        env=process_env,
                        text=True,
                        capture_output=True,
                    )
                    self.assertEqual(
                        result.returncode, 0, result.stdout + result.stderr
                    )
                    self.assertIn(
                        "write_flash 0x10000 " + str(image), calls.read_text()
                    )
                    self.assertNotIn("erase", calls.read_text())
                    calls.unlink()
                    image.write_bytes(b"corrupted-image")
                    result = subprocess.run(
                        [
                            "bash",
                            str(ROOT / "bin/device-update.sh"),
                            "-p",
                            "/dev/null",
                            "-P",
                            str(stub),
                            "-f",
                            str(image),
                        ],
                        env=process_env,
                        text=True,
                        capture_output=True,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(calls.exists())

    def hooks(self, target="heltec-v4-standard"):
        env = BuildEnv(self.root, target)
        globals_ = {"env": env, "projenv": env, "Import": lambda name: None}
        runpy.run_path(str(ROOT / "bin/platformio-pre.py"), init_globals=globals_)
        module = runpy.run_path(
            str(ROOT / "bin/platformio-custom.py"), init_globals=globals_
        )
        return env, module

    def test_actual_hooks_bind_api_filename_sdk_cache_epoch_and_manifest(self):
        for target in ("heltec-v4-standard", "heltec-v4-solar-router"):
            with self.subTest(target=target):
                env, module = self.hooks(target)
                version = self.version()
                self.assertEqual(
                    env["PROGNAME"], f"firmware-{target}-{version['long']}"
                )
                self.assertIn("-DAPP_VERSION=" + version["long"], env["CCFLAGS"])
                self.assertIn("-DAPP_VERSION_SHORT=" + version["short"], env["CCFLAGS"])
                self.assertIn(
                    "-DBUILD_EPOCH=" + str(version["build_epoch"]), env["CCFLAGS"]
                )
                self.assertEqual(
                    env["ENV"]["SOURCE_DATE_EPOCH"], str(version["build_epoch"])
                )
                sdkconfig = env.GetProjectOption("custom_sdkconfig")
                self.assertIn(
                    'CONFIG_APP_PROJECT_VER="' + version["long"] + '"', sdkconfig
                )
                self.assertIn("CONFIG_APP_PROJECT_VER_FROM_CONFIG=y", sdkconfig)
                self.assertIn("CONFIG_APP_REPRODUCIBLE_BUILD=y", sdkconfig)
                self.assertIn("CONFIG_SPIRAM=y", sdkconfig)
                image = env.build_dir / (env["PROGNAME"] + ".bin")
                payload = b"firmware-test\0" + version["long"].encode()
                image.write_bytes(payload)
                module["manifest_gather"](None, None, env)
                manifest = json.loads(
                    (env.build_dir / (env["PROGNAME"] + ".mt.json")).read_text()
                )
                self.assertEqual(manifest["source"], version["source"])
                self.assertEqual(manifest["version"], version["long"])
                self.assertEqual(
                    manifest["identity_sha256"], version["identity_sha256"]
                )
                self.assertEqual(manifest["upstream_version"], "2.8.0")
                self.assertEqual(manifest["fork_revision"], 9)
                self.assertEqual(
                    manifest["files"],
                    [
                        {
                            "name": image.name,
                            "bytes": len(payload),
                            "md5": hashlib.md5(payload).hexdigest(),
                            "sha256": hashlib.sha256(payload).hexdigest(),
                            "part_name": "app0",
                        }
                    ],
                )

    def test_manifest_rejects_sources_changed_after_pre_hook(self):
        env, module = self.hooks()
        self.write("main.cpp", "int value = 99;\n")
        with self.assertRaisesRegex(RuntimeError, "sources changed"):
            module["manifest_gather"](None, None, env)
        self.assertFalse(list(env.build_dir.glob("*.mt.json")))

    def test_installer_version_grammars_accept_new_version(self):
        version = self.version()["long"]
        for name in (
            "device-install.sh",
            "device-update.sh",
            "device-install.bat",
            "device-update.bat",
        ):
            text = (ROOT / "bin" / name).read_text()
            pattern = re.search(r"\^\[0-9A-Za-z\].*?\$", text).group()
            self.assertIsNotNone(re.fullmatch(pattern, version), name)


if __name__ == "__main__":
    unittest.main()
