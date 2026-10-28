#!/usr/bin/env python3
"""Check stripped runtime identities and package/variant/profile guards before any ADB action."""
import hashlib
import json
import os
import subprocess
import zipfile
from pathlib import Path

from deploy_totk import PERFTEST_PACKAGE, select_target, validate_library
from package_android import apk_application_id, package, runtime_identity

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/android-package-tests"
NDK = Path(os.environ.get("ANDROID_NDK", "/home/deck/Code/Toolchains/android-ndk-r30"))
BIN = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT.mkdir(parents=True, exist_ok=True)


def refused(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError("Mismatched build was accepted")


# Use the production declarations, with real cross-compiled ELF program/dynamic-symbol tables.
source = (ROOT / "runtime/gamelib.c").read_text()
declarations = "\n".join(line for line in source.splitlines() if line.startswith("GAMELIB_EXPORT const char gamelib_runtime_"))
(OUT / "runtime.c").write_text('#define GAMELIB_EXPORT __attribute__((visibility("default")))\n' + declarations + "\n")
identity = "0123456789abcdef"
for profile in ("dev", "performance", "snapdragon-8elite"):
    library = OUT / f"{profile}.so"
    subprocess.run([str(BIN / "clang"), "--target=aarch64-linux-android30", "-O3", "-flto=thin",
                    "-shared", "-nostdlib", "-fPIC", f'-DGAMELIB_ID="{identity}"',
                    f'-DGAMELIB_BUILD_PROFILE="{profile}"', str(OUT / "runtime.c"), "-o", str(library)], check=True)
    subprocess.run([str(BIN / "llvm-strip"), "--strip-all", str(library)], check=True)
    assert runtime_identity(library.read_bytes()) == (identity, profile)
refused(lambda: runtime_identity((OUT / "dev.so").read_bytes()[:100]))

metadata = OUT / "output-metadata.json"
apk = OUT / "input.apk"
runtime = (OUT / "snapdragon-8elite.so").read_bytes()
sdk = Path(os.environ.get("ANDROID_HOME", "/home/deck/Code/Toolchains/android-sdk"))
aapt2 = sdk / "build-tools/36.0.0/aapt2"
manifest = OUT / "AndroidManifest.xml"


def make_apk(path, application_id):
    manifest.write_text(f'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{application_id}">'
                        '<application android:label="Package guard test"/></manifest>\n')
    subprocess.run([str(aapt2), "link", "--manifest", str(manifest), "-I", str(sdk / "platforms/android-36/android.jar"),
                    "-o", str(path)], check=True)
    with zipfile.ZipFile(path, "a") as archive:
        archive.writestr("lib/arm64-v8a/libmain.so", runtime)


make_apk(apk, PERFTEST_PACKAGE)
assert apk_application_id(apk) == PERFTEST_PACKAGE
with zipfile.ZipFile(apk, "a") as archive:
    archive.writestr("assets/test.txt", "No game data")
metadata.write_text(json.dumps(dict(applicationId=PERFTEST_PACKAGE, variantName="release",
                                    elements=[dict(outputFile=apk.name, versionName="test")])) + "\n")
staged = OUT / "staged"
package(metadata, staged, "release", "snapdragon-8elite", PERFTEST_PACKAGE)
record = json.loads((staged / "build.json").read_text())
assert record["gamelib_id"] == identity
assert record["runtime_sha256"] == hashlib.sha256(runtime).hexdigest()
assert select_target(staged / record["apk"]) == ("release", PERFTEST_PACKAGE, "snapdragon-8elite")
assert select_target(apk, package=PERFTEST_PACKAGE) == ("release", PERFTEST_PACKAGE, None)
refused(lambda: package(metadata, staged, "release", "performance", PERFTEST_PACKAGE))
refused(lambda: package(metadata, staged, "debug", "snapdragon-8elite", PERFTEST_PACKAGE))
refused(lambda: package(metadata, staged, "release", "snapdragon-8elite"))
refused(lambda: select_target(apk, "debug", PERFTEST_PACKAGE))
refused(lambda: select_target(apk, package="com.totkrecomp.android"))
assert select_target(variant="debug")[:2] == ("debug", "com.totkrecomp.android.debug")
assert select_target(variant="release", package=PERFTEST_PACKAGE)[:2] == ("release", PERFTEST_PACKAGE)
refused(lambda: select_target(variant="release", package="com.totkrecomp.android.debug"))

# A stale adjacent metadata file must never turn an ordinary APK into an authorized perftest install.
make_apk(apk, "com.totkrecomp.android")
refused(lambda: select_target(apk, "release", PERFTEST_PACKAGE))
refused(lambda: package(metadata, staged, "release", "snapdragon-8elite", PERFTEST_PACKAGE))
make_apk(apk, PERFTEST_PACKAGE)

(staged / "totkgame.so").write_bytes(b"placeholder")
(staged / "totkgame.so.game").write_text("143\n")
(staged / "totkgame.so.id").write_text(identity + "\n")
(staged / "totkgame.so.profile").write_text("snapdragon-8elite\n")
validate_library(staged, "snapdragon-8elite", identity)
refused(lambda: validate_library(staged, "performance", identity))
refused(lambda: validate_library(staged, "snapdragon-8elite", "fedcba9876543210"))
(staged / "totkgame.so.game").write_text("182\n")
refused(lambda: validate_library(staged, "snapdragon-8elite", identity))
print("Stripped ELF, APK metadata, perftest targeting and game-library identity checks passed")
