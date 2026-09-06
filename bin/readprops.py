import base64
import configparser
import hashlib
import os
import subprocess
from pathlib import Path

MAX_FIRMWARE_VERSION_BYTES = 17
MIN_FINGERPRINT_CHARACTERS = 6


def _git(root, *args):
    env = os.environ.copy()
    if (root / ".git").exists():
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            env.pop(key, None)
    return subprocess.check_output(
        ["git", "-C", str(root), *args], env=env, stderr=subprocess.PIPE
    )


def _field(digest, value):
    digest.update(len(value).to_bytes(8, "big"))
    digest.update(value)


def _source_state(root):
    commit = _git(root, "rev-parse", "HEAD").decode().strip()
    epoch = int(_git(root, "show", "-s", "--format=%ct", "HEAD"))
    entries = {}
    for row in _git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if row:
            metadata, path = row.split(b"\t", 1)
            mode, oid, stage = metadata.split()
            if stage != b"0":
                raise ValueError(
                    "Cannot version a source tree with unresolved conflicts"
                )
            entries[path] = (mode, oid)
    untracked = _git(root, "ls-files", "--others", "--exclude-standard", "-z").split(
        b"\0"
    )
    for path in filter(None, untracked):
        entries[path] = (b"100644", b"")
    changed = _git(
        root, "diff", "HEAD", "--name-only", "-z", "--ignore-submodules=none"
    )
    dirty = bool(changed or any(untracked))
    digest = hashlib.sha256(b"heltec-source-v1\0")
    _field(digest, commit.encode())
    for name, (mode, oid) in sorted(entries.items()):
        path = root / os.fsdecode(name)
        if mode != b"160000" and not path.exists() and not path.is_symlink():
            continue
        _field(digest, name)
        _field(digest, mode)
        if mode == b"160000":
            # Clean initialized and uninitialized submodules share their pinned identity.
            content = oid
            if (path / ".git").exists():
                nested = _source_state(path)
                content = nested["commit"].encode()
                if nested["dirty"]:
                    content += b":" + nested["sha256"].encode()
                dirty |= nested["dirty"] or content != oid
        elif path.is_symlink():
            content = os.fsencode(os.readlink(path))
        elif path.exists():
            content = path.read_bytes()
            # Match the repository's text normalization across Windows and Linux.
            if b"\0" not in content:
                content = content.replace(b"\r\n", b"\n")
        _field(digest, content)
    return {
        "algorithm": "heltec-source-v1",
        "commit": commit,
        "dirty": dirty,
        "sha256": digest.hexdigest(),
        "commit_epoch": epoch,
    }


def readProps(prefsLoc):
    """Use one deterministic source identity for the API, artifacts and manifest."""
    prefs = Path(prefsLoc).resolve()
    config = configparser.RawConfigParser()
    with prefs.open(encoding="utf-8") as source:
        config.read_file(source)
    version = config["VERSION"]
    upstream = ".".join(str(int(version[key])) for key in ("major", "minor", "build"))
    revision = int(version["fork_revision"])
    if revision < 1:
        raise ValueError("fork_revision must be positive")
    try:
        source = _source_state(prefs.parent)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError(
            "A readable Git checkout is required to identify firmware sources"
        ) from exc
    epoch = int(os.getenv("SOURCE_DATE_EPOCH", str(source["commit_epoch"])))
    if not 0 <= epoch <= 0xFFFFFFFF:
        raise ValueError("SOURCE_DATE_EPOCH must fit an unsigned 32-bit Unix epoch")
    # The digest includes the epoch because BUILD_EPOCH is compiled into the image.
    identity = hashlib.sha256(f"{source['sha256']}:{epoch}".encode()).hexdigest()
    state = "d" if source["dirty"] else "g"
    short = f"{upstream}-h{revision}"
    prefix = f"{short}{state}"
    fingerprint_length = min(
        8, MAX_FIRMWARE_VERSION_BYTES - len(prefix.encode("ascii"))
    )
    if fingerprint_length < MIN_FINGERPRINT_CHARACTERS:
        raise ValueError(
            f"Firmware prefix {prefix!r} leaves fewer than 6 fingerprint characters within the API's 17-byte limit"
        )
    fingerprint = base64.b32encode(bytes.fromhex(identity)).decode("ascii").lower()
    long = prefix + fingerprint[:fingerprint_length]
    return {
        "short": short,
        "long": long,
        "deb": long,
        "upstream": upstream,
        "fork_revision": revision,
        "build_epoch": epoch,
        "source": source,
        "identity_sha256": identity,
    }


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
