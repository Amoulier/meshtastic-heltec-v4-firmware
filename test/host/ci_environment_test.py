#!/usr/bin/env python3
"""Check local/CI environment parity and real Git snapshot preservation."""

import ast
import importlib.util
import json
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ci_snapshot", ROOT / "bin/ci-snapshot.py"
)
SNAPSHOT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SNAPSHOT)


class EnvironmentParityTests(unittest.TestCase):
    def test_github_jobs_use_the_same_runners_as_local_validation(self):
        firmware = (ROOT / ".github/workflows/build_firmware.yml").read_text()
        native = (ROOT / ".github/workflows/audit_native_heltec.yml").read_text()
        self.assertRegex(firmware, r"(?m)^\s+run: bash bin/heltec-ci\.sh ")
        self.assertRegex(native, r"(?m)^\s+run: bash bin/test-native-docker\.sh ")
        for workflow in (firmware, native):
            self.assertIn("submodules: recursive", workflow)
            self.assertIn("persist-credentials: false", workflow)
            self.assertNotIn("ubuntu-latest", workflow)

    def test_devcontainer_uses_ci_image_without_toolchain_overrides(self):
        values = dict(
            line.split("=", 1)
            for line in (ROOT / "bin/heltec-ci.env").read_text().splitlines()
            if line and not line.startswith("#")
        )
        config = json.loads((ROOT / ".devcontainer/devcontainer.json").read_text())
        self.assertEqual(config["image"], values["HELTEC_BUILD_IMAGE"])
        self.assertRegex(config["image"], r"@sha256:[0-9a-f]{64}$")
        self.assertNotIn("build", config)
        self.assertNotIn("features", config)
        self.assertEqual(config["workspaceFolder"], "/workspace")
        self.assertTrue(config["overrideCommand"])
        self.assertEqual(config["remoteUser"], "root")
        self.assertEqual(
            config["postCreateCommand"],
            ["git", "submodule", "update", "--init", "--recursive"],
        )
        for target in ("/workspace/.pio", "/pio/core", "/pio/workspace"):
            self.assertTrue(
                any(
                    f"target={target}," in mount and "type=volume" in mount
                    for mount in config["mounts"]
                )
            )

    def test_default_editor_task_runs_complete_validation_on_both_hosts(self):
        tasks = json.loads((ROOT / ".vscode/tasks.json").read_text())["tasks"]
        defaults = [
            task
            for task in tasks
            if isinstance(task.get("group"), dict) and task["group"].get("isDefault")
        ]
        self.assertEqual(len(defaults), 1)
        task = defaults[0]
        self.assertEqual(task["command"], "bash")
        self.assertEqual(task["args"], ["bin/heltec-ci.sh", "all"])
        self.assertEqual(task["windows"]["command"], "powershell.exe")
        self.assertEqual(
            task["windows"]["args"][-2:],
            ["${workspaceFolder}/bin/heltec-ci.ps1", "all"],
        )


class SourceSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="heltec-ci-snapshot-")
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / "source with spaces"
        self.destination = self.base / "snapshot with spaces"
        self.source.mkdir()
        env = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith("GIT_") and key != "SOURCE_DATE_EPOCH"
        }
        env.update(
            GIT_AUTHOR_DATE="2026-09-01T00:00:00Z",
            GIT_COMMITTER_DATE="2026-09-01T00:00:00Z",
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
        )
        environment = patch.dict(os.environ, env, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.initialize(self.source)
        self.write(".gitattributes", "* text=auto eol=lf\n")
        self.write(".gitignore", ".pio/\naudit-results/\n__pycache__/\nprivate/\n")
        self.write(
            "version.properties",
            "[VERSION]\nmajor=2\nminor=8\nbuild=0\nfork_revision=9\n",
        )
        self.write("bin/readprops.py", (ROOT / "bin/readprops.py").read_bytes())
        self.write("main.cpp", "int value = 1;\n")
        self.write("remove.cpp", "int removed = 1;\n")
        self.write("run.sh", "#!/bin/sh\nexit 0\n")
        self.git(self.source, "add", ".")
        self.git(self.source, "update-index", "--chmod=+x", "run.sh")
        self.git(self.source, "commit", "-qm", "fixture")

    @staticmethod
    def git(root, *args):
        return subprocess.check_output(
            ["git", "-C", str(root), *args], stderr=subprocess.PIPE
        )

    def initialize(self, root):
        self.git(root, "init", "--initial-branch=main", "-q")
        self.git(root, "config", "user.name", "Snapshot Test")
        self.git(root, "config", "user.email", "test@example.invalid")
        self.git(root, "config", "core.autocrlf", "false")
        self.git(root, "config", "core.filemode", "false")

    def write(self, name, content):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode() if isinstance(content, str) else content)

    def assert_snapshot(self):
        before = SNAPSHOT.identity(self.source)
        index = self.git(self.source, "ls-files", "--stage", "-z")
        SNAPSHOT.snapshot(self.source, self.destination)
        self.assertEqual(SNAPSHOT.identity(self.source), before)
        self.assertEqual(SNAPSHOT.identity(self.destination), before)
        self.assertEqual(self.git(self.destination, "ls-files", "--stage", "-z"), index)
        self.assertEqual(self.git(self.source, "ls-files", "--stage", "-z"), index)
        return before

    def test_clean_snapshot_keeps_version_executable_files_and_ignores_backups(self):
        self.write(".pio/verification/private-node.pb", b"synthetic private backup\0")
        self.write("private/node.pb", b"synthetic private backup\0")
        self.write("audit-results/summary.json", "{}\n")
        before = self.assert_snapshot()
        self.assertFalse(before["source"]["dirty"])
        for path in (".pio", "private", "audit-results"):
            self.assertFalse((self.destination / path).exists())
        self.assertNotIn(
            "origin", self.git(self.destination, "remote").decode().splitlines()
        )
        self.assertNotIn(
            self.source.as_uri(), (self.destination / ".git/config").read_text()
        )
        if os.name == "posix":
            self.assertTrue((self.destination / "run.sh").stat().st_mode & stat.S_IXUSR)

    def test_origin_preserves_actual_platformio_repository_provenance(self):
        origin = "https://github.com/Amoulier/meshtastic-heltec-v4-firmware.git"
        self.git(self.source, "remote", "add", "origin", origin)
        self.assert_snapshot()
        self.assertEqual(
            self.git(self.destination, "config", "--get", "remote.origin.url")
            .decode()
            .strip(),
            origin,
        )
        self.assertNotIn(
            self.source.as_uri(), (self.destination / ".git/config").read_text()
        )
        self.assertFalse((self.destination / ".git/objects/info/alternates").exists())
        self.assertEqual(
            self.git(self.destination, "show", "HEAD:main.cpp"), b"int value = 1;\n"
        )
        tree = ast.parse((ROOT / "bin/platformio-custom.py").read_text())
        provenance = next(
            node
            for node in tree.body
            if isinstance(node, ast.Try)
            and any(
                isinstance(child, ast.Name) and child.id == "repo_owner"
                for child in ast.walk(node)
            )
        )
        context = {
            "subprocess": subprocess,
            "projenv": {"PROJECT_DIR": str(self.destination)},
        }
        exec(
            compile(
                ast.Module(body=[provenance], type_ignores=[]),
                "platformio-custom.py",
                "exec",
            ),
            context,
        )
        self.assertEqual(
            context["repo_owner"], "Amoulier/meshtastic-heltec-v4-firmware"
        )

    def test_origin_does_not_copy_embedded_credentials_into_snapshot(self):
        self.git(
            self.source,
            "remote",
            "add",
            "origin",
            "https://synthetic-user:synthetic-token@github.com/example/firmware.git?credential=synthetic-query#synthetic-fragment",
        )
        self.assert_snapshot()
        self.assertEqual(
            self.git(self.destination, "config", "--get", "remote.origin.url")
            .decode()
            .strip(),
            "https://github.com/example/firmware.git",
        )
        self.assertNotIn("synthetic-", (self.destination / ".git/config").read_text())

    def test_staged_and_unstaged_sources_preserve_index_and_exact_working_content(self):
        self.write("main.cpp", "int value = 2;\n")
        self.git(self.source, "add", "main.cpp")
        self.write("main.cpp", "int value = 3;\r\n")
        self.write("new.cpp", "int added = 1;\n")
        self.git(self.source, "add", "new.cpp")
        self.write("new.cpp", "int added = 2;\n")
        self.write("untracked.cpp", "int untracked = 1;\n")
        self.write("binary.dat", b"\0\r\n")
        self.git(self.source, "rm", "-q", "remove.cpp")
        before = self.assert_snapshot()
        self.assertTrue(before["source"]["dirty"])
        self.assertEqual(
            (self.destination / "main.cpp").read_bytes(), b"int value = 3;\n"
        )
        self.assertEqual((self.destination / "binary.dat").read_bytes(), b"\0\r\n")
        self.assertTrue((self.destination / "untracked.cpp").exists())
        self.assertFalse((self.destination / "remove.cpp").exists())

    def test_unstaged_deletion_and_staged_executable_change_are_preserved(self):
        (self.source / "remove.cpp").unlink()
        self.git(self.source, "update-index", "--chmod=+x", "main.cpp")
        self.assert_snapshot()
        self.assertFalse((self.destination / "remove.cpp").exists())
        if os.name == "posix":
            self.assertTrue(
                (self.destination / "main.cpp").stat().st_mode & stat.S_IXUSR
            )

    @unittest.skipUnless(os.name == "posix", "Linux executable-bit semantics")
    def test_unstaged_executable_change_is_preserved_when_git_tracks_modes(self):
        (self.source / "run.sh").chmod(0o755)
        self.git(self.source, "config", "core.filemode", "true")
        (self.source / "main.cpp").chmod(0o755)
        self.assert_snapshot()
        self.assertTrue((self.destination / "main.cpp").stat().st_mode & stat.S_IXUSR)

    def test_checkout_applies_declared_line_endings_and_preserves_binary_bytes(self):
        self.write(
            ".gitattributes",
            "* text=auto eol=lf\n*.bat text eol=crlf\n*.cmd text eol=crlf\n"
            "*.ps1 text eol=crlf\n*.dat -text\n",
        )
        for name in ("install.bat", "install.cmd", "install.ps1", "install.sh"):
            self.write(name, b"first\nsecond\n")
        self.write("literal.dat", b"first\r\nsecond\n")
        self.write("binary.bat", b"first\0\nsecond\n")
        self.git(self.source, "add", ".")
        self.assert_snapshot()
        for name in ("install.bat", "install.cmd", "install.ps1"):
            self.assertEqual(
                (self.destination / name).read_bytes(), b"first\r\nsecond\r\n"
            )
        self.assertEqual(
            (self.destination / "install.sh").read_bytes(), b"first\nsecond\n"
        )
        self.assertEqual(
            (self.destination / "literal.dat").read_bytes(), b"first\r\nsecond\n"
        )
        self.assertEqual(
            (self.destination / "binary.bat").read_bytes(), b"first\0\nsecond\n"
        )

    def test_dirty_submodule_keeps_local_sources_without_remote_fetches(self):
        upstream = self.base / "submodule origin"
        upstream.mkdir()
        self.initialize(upstream)
        (upstream / "schema.proto").write_text("message Original {}\n")
        self.git(upstream, "add", ".")
        self.git(upstream, "commit", "-qm", "schema fixture")
        self.git(
            self.source,
            "-c",
            "protocol.file.allow=always",
            "submodule",
            "add",
            "--quiet",
            str(upstream),
            "protobufs",
        )
        self.git(self.source, "commit", "-qm", "add submodule")
        submodule = self.source / "protobufs"
        (submodule / "schema.proto").write_text("message Staged {}\n")
        self.git(submodule, "add", "schema.proto")
        (submodule / "schema.proto").write_text("message Working {}\n")
        (submodule / "extra.proto").write_text("message Extra {}\n")
        (submodule / ".gitignore").write_text("private/\n")
        (submodule / "private").mkdir()
        (submodule / "private/node.pb").write_bytes(b"synthetic private backup\0")
        before = self.assert_snapshot()
        self.assertTrue(before["source"]["dirty"])
        self.assertEqual(
            self.git(submodule, "ls-files", "--stage", "-z"),
            self.git(self.destination / "protobufs", "ls-files", "--stage", "-z"),
        )
        self.assertFalse((self.destination / "protobufs/private").exists())

    def test_nonempty_destination_is_rejected_without_modifying_it(self):
        self.destination.mkdir()
        preserved = self.destination / "preserve.txt"
        preserved.write_text("keep\n")
        with self.assertRaisesRegex(ValueError, "must be empty"):
            SNAPSHOT.snapshot(self.source, self.destination)
        self.assertEqual(preserved.read_text(), "keep\n")


if __name__ == "__main__":
    unittest.main()
