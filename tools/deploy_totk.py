"""Deploy prepared TotK inputs to this app, keeping Games/TotK originals intact."""
import argparse
import json
import subprocess
from pathlib import Path

from totk import inspect

PACKAGE = "com.totkrecomp.android"
ACTIVITY = PACKAGE + "/com.botwrecompandroid.BOTWRecompAndroidActivity"
DEST = "/storage/emulated/0/Android/data/" + PACKAGE + "/files/extracted"


def deploy(game, library, serial, adb, apk=None, launch=False, data_only=False):
    game, library = Path(game), Path(library)
    report = inspect(game / "exefs")
    romfs = game / "romfs.bin"
    manifest = json.loads((game / "manifest.json").read_text())
    if manifest["modules"] != report["modules"] or manifest["romfs_bytes"] != romfs.stat().st_size:
        raise ValueError("prepared inputs differ from their manifest")
    if data_only and launch:
        raise ValueError("--data-only cannot be combined with --launch")
    if not data_only:
        for name in ("totkgame.so", "totkgame.so.id", "totkgame.so.game"):
            if not (library / name).is_file():
                raise ValueError(f"missing {library / name}")
        if (library / "totkgame.so.game").read_text().strip() != "143":
            raise ValueError("game library targets another version")
    prefix = [adb, "-s", serial]
    def call(*args, **kw):
        return subprocess.run(prefix + list(args), check=True, **kw)
    if apk:
        call("install", "-r", str(apk))  # streamed PackageInstaller session
    call("shell", "am", "force-stop", PACKAGE)
    call("shell", "mkdir", "-p", DEST)
    remote = subprocess.run(prefix + ["shell", "cat", DEST + "/manifest.json"], capture_output=True, text=True)
    size = subprocess.run(prefix + ["shell", "stat", "-c", "%s", DEST + "/romfs.bin"], capture_output=True, text=True)
    current = False
    try:
        current = json.loads(remote.stdout) == manifest and int(size.stdout.strip()) == romfs.stat().st_size
    except (ValueError, TypeError):
        pass
    if not current:
        call("push", "-Z", str(romfs), DEST + "/romfs.bin.part")
        got = call("shell", "stat", "-c", "%s", DEST + "/romfs.bin.part", capture_output=True, text=True)
        if int(got.stdout.strip()) != romfs.stat().st_size:
            raise RuntimeError("device RomFS transfer is incomplete")
        call("shell", "mv", DEST + "/romfs.bin.part", DEST + "/romfs.bin")
        call("push", str(game / "exefs"), DEST + "/")
        call("push", str(game / "manifest.json"), DEST + "/manifest.json")
    # FFmpeg opens movies by path, whereas the game's own filesystem reads the image.
    if (game / "romfs").is_dir():
        call("push", "--sync", str(game / "romfs"), DEST + "/")
    if not data_only:
        call("push", *(str(library / n) for n in ("totkgame.so", "totkgame.so.id", "totkgame.so.game")), DEST + "/")
    call("shell", "chmod", "-R", "o+rX", DEST)
    if launch:
        call("shell", "am", "start", "-n", ACTIVITY, "--es", "env", "LARECOMP_NO_LAUNCHER=1")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("game", type=Path)
    p.add_argument("--library", type=Path, default=Path("android/out"))
    p.add_argument("--serial", required=True)
    p.add_argument("--adb", default="/home/deck/Code/Toolchains/android-sdk/platform-tools/adb")
    p.add_argument("--apk", type=Path)
    p.add_argument("--launch", action="store_true")
    p.add_argument("--data-only", action="store_true", help="Stage large game inputs while the game library builds")
    a = p.parse_args()
    deploy(a.game, a.library, a.serial, a.adb, a.apk, a.launch, a.data_only)
