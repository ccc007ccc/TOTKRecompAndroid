"""Deploy prepared TotK inputs to this app, keeping Games/TotK originals intact."""
import argparse
import hashlib
import json
import subprocess
import zipfile
from pathlib import Path

from package_android import PROFILES, apk_application_id, runtime_identity
from totk import inspect

ROOT = Path(__file__).resolve().parents[1]
PACKAGES = {"debug": "com.totkrecomp.android.debug", "release": "com.totkrecomp.android"}
PERFTEST_PACKAGE = "com.totkrecomp.android.perftest"
VALID_PACKAGES = {*PACKAGES.values(), PERFTEST_PACKAGE}


def select_target(apk=None, variant=None, package=None):
    info = {}
    if apk:
        apk = Path(apk)
        for filename in ("build.json", "output-metadata.json"):
            metadata = apk.parent / filename
            if metadata.is_file():
                info = json.loads(metadata.read_text())
                break
        actual_package = info.get("application_id", info.get("applicationId"))
        manifest_package = apk_application_id(apk)
        if actual_package and manifest_package != actual_package:
            raise ValueError("Actual APK package does not match its metadata")
        actual_package = manifest_package
        if info.get("apk_sha256"):
            with apk.open("rb") as stream:
                if hashlib.file_digest(stream, "sha256").hexdigest() != info["apk_sha256"]:
                    raise ValueError("APK hash does not match its build metadata")
        actual_variant = info.get("variant", info.get("variantName"))
        if actual_package:
            if actual_package not in VALID_PACKAGES or (package is not None and package != actual_package):
                raise ValueError("APK package does not match the requested TOTK package")
            package = actual_package
            if actual_variant is None:
                actual_variant = next((key for key, value in PACKAGES.items() if value == package), None)
            if actual_variant is not None and variant is not None and variant != actual_variant:
                raise ValueError("APK package does not match the requested TOTK variant")
            variant = actual_variant or variant
        if variant is None:
            raise ValueError("Perftest APK metadata needs a variant; specify --variant")
    variant = variant or "debug"
    if variant not in PACKAGES:
        raise ValueError("variant must be debug or release")
    package = package or PACKAGES[variant]
    if package not in VALID_PACKAGES or (package != PERFTEST_PACKAGE and package != PACKAGES[variant]):
        raise ValueError("TOTK package does not match the requested variant")
    return variant, package, info.get("profile")


def validate_library(library, profile, identity=None):
    library = Path(library)
    for name in ("totkgame.so", "totkgame.so.id", "totkgame.so.game"):
        if not (library / name).is_file():
            raise ValueError(f"missing {library / name}")
    if (library / "totkgame.so.game").read_text().strip() != "143":
        raise ValueError("game library targets another version")
    profile_file = library / "totkgame.so.profile"
    library_profile = profile_file.read_text().strip() if profile_file.is_file() else "dev"
    if profile != library_profile:
        raise ValueError("APK and game library use different build profiles; rebuild the matching game library")
    if identity is not None and (library / "totkgame.so.id").read_text().strip() != identity:
        raise ValueError("APK and game library have different GAMELIB_IDs; rebuild the matching game library")


def deploy(game, library, serial, adb, apk=None, launch=False, data_only=False, variant=None, package=None, profile=None):
    variant, package, metadata_profile = select_target(apk, variant, package)
    identity = None
    runtime_profile = None
    if apk:
        with zipfile.ZipFile(apk) as archive:
            identity, runtime_profile = runtime_identity(archive.read("lib/arm64-v8a/libmain.so"))
    if metadata_profile and profile and metadata_profile != profile:
        raise ValueError("APK metadata does not match the requested build profile")
    profile = profile or metadata_profile or runtime_profile or ("dev" if variant == "debug" else "performance")
    if profile not in PROFILES:
        raise ValueError("Unknown TOTK build profile")
    game = Path(game)
    suffix = "-perftest" if package == PERFTEST_PACKAGE else ""
    library = Path(library) if library else ROOT / ".work/totk-port/packages" / f"{variant}-{profile}{suffix}"
    activity = package + "/com.botwrecompandroid.BOTWRecompAndroidActivity"
    dest = "/storage/emulated/0/Android/data/" + package + "/files/extracted"
    report = inspect(game / "exefs")
    romfs = game / "romfs.bin"
    manifest = json.loads((game / "manifest.json").read_text())
    if manifest["modules"] != report["modules"] or manifest["romfs_bytes"] != romfs.stat().st_size:
        raise ValueError("prepared inputs differ from their manifest")
    if data_only and launch:
        raise ValueError("--data-only cannot be combined with --launch")
    if apk:
        if profile != runtime_profile:
            raise ValueError("APK runtime does not match the requested build profile")
    elif not data_only and (library / "libmain.so").is_file():
        identity, runtime_profile = runtime_identity((library / "libmain.so").read_bytes())
        if profile != runtime_profile:
            raise ValueError("Packaged runtime does not match the requested build profile")
    if not data_only:
        validate_library(library, profile, identity)
    prefix = [adb, "-s", serial]
    def call(*args, **kw):
        return subprocess.run(prefix + list(args), check=True, **kw)
    if apk:
        call("install", "--streaming", "-r", str(apk))
    call("shell", "am", "force-stop", package)
    call("shell", "mkdir", "-p", dest)
    remote = subprocess.run(prefix + ["shell", "cat", dest + "/manifest.json"], capture_output=True, text=True)
    size = subprocess.run(prefix + ["shell", "stat", "-c", "%s", dest + "/romfs.bin"], capture_output=True, text=True)
    current = False
    try:
        current = json.loads(remote.stdout) == manifest and int(size.stdout.strip()) == romfs.stat().st_size
    except (ValueError, TypeError):
        pass
    if not current:
        call("push", "-Z", str(romfs), dest + "/romfs.bin.part")
        got = call("shell", "stat", "-c", "%s", dest + "/romfs.bin.part", capture_output=True, text=True)
        if int(got.stdout.strip()) != romfs.stat().st_size:
            raise RuntimeError("device RomFS transfer is incomplete")
        call("shell", "mv", dest + "/romfs.bin.part", dest + "/romfs.bin")
        call("push", str(game / "exefs"), dest + "/")
        call("push", str(game / "manifest.json"), dest + "/manifest.json")
    # FFmpeg opens movies by path, whereas the game's own filesystem reads the image.
    if (game / "romfs").is_dir():
        call("push", "--sync", str(game / "romfs"), dest + "/")
    if not data_only:
        call("push", *(str(library / n) for n in ("totkgame.so", "totkgame.so.id", "totkgame.so.game")), dest + "/")
    call("shell", "chmod", "-R", "o+rX", dest)
    if launch:
        call("shell", "am", "start", "-n", activity, "--es", "env", "LARECOMP_NO_LAUNCHER=1")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("game", type=Path)
    p.add_argument("--library", type=Path, help="Defaults to the selected variant/profile package directory")
    p.add_argument("--serial", required=True)
    p.add_argument("--adb", default="/home/deck/Code/Toolchains/android-sdk/platform-tools/adb")
    p.add_argument("--apk", type=Path)
    p.add_argument("--variant", choices=PACKAGES, help="Inferred from APK metadata; defaults to debug without an APK")
    p.add_argument("--package", choices=sorted(VALID_PACKAGES), help="Explicit application package, including perftest")
    p.add_argument("--profile", choices=PROFILES, help="Inferred from APK metadata; defaults to the variant's profile")
    p.add_argument("--launch", action="store_true")
    p.add_argument("--data-only", action="store_true", help="Stage large game inputs while the game library builds")
    a = p.parse_args()
    deploy(a.game, a.library, a.serial, a.adb, a.apk, a.launch, a.data_only, a.variant, a.package, a.profile)
