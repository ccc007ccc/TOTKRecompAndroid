# Contributing to TOTKRecompAndroid

[简体中文](CONTRIBUTING.md) | English

This branch targets TOTK Switch 1.4.3 on Android ARM64. Its direct upstream is
[mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid).
Read the [status](docs/en/STATUS.md), [architecture](docs/en/ARCHITECTURE.md) and [upstream notes](docs/en/UPSTREAM.md).

## Contribution scope

Source, tests with synthetic inputs and documentation are welcome. Handwritten and AI-assisted changes have the same validation requirements.
Chinese is the default documentation language; keep the English guides aligned. Match nearby code style and explain constraints and reasons.

Do not commit game packages, NSO/NCA, RomFS, keys, generated game C/shaders, proprietary fonts, textures, audio or saves.
Keep raw device captures and replay inputs under the ignored `.work/totk-port/` directory.
Relevant screenshots, short clips and logs without private data can accompany issue reports.

## Development and verification

SteamOS is immutable. Run builds, dependency installation, generators and tests inside `distrobox enter dev`.
Source `android/dev_env.sh` from the project root. Temporary data stays inside the project; toolchains default to `/home/deck/Code/Toolchains/`.
See [building](docs/en/BUILD.md) and [testing](docs/en/TESTING.md).

- Preserve four-module identity checks, application data isolation and confirmed character fixes.
- Do not apply BOTW/LAS addresses or game-specific patches to TOTK without verification.
- Run checks relevant to the change; graphics fixes need device or independent replay evidence with stated limits.
- A successful build, a few normal frames and user confirmation are different forms of evidence. Preserve negative results and user feedback.
- Investigate source and existing captures before restarting. F9 can trigger another capture in an appropriately configured process.
- Record the APK/commit, driver, environment, scene and valid frame range. Readback diagnostics are not performance tests.
- Reset per-boot state when the game restarts; follow `RT_ON_RESET` and backend reset handling.

## Issues and pull requests

Use [this fork's issue tracker](https://github.com/ccc007ccc/TOTKRecompAndroid/issues).
Include the device, Android version, GPU/driver, game revision, steps, expected result and actual result.
The Android log tag is `TOTKRecompAndroid`.

Describe the concrete problem, final behavior, validation and remaining limitations. Preserve provenance and license notices.
After merging upstream, review game identity, resource checks, UI wording and GPU counters: a clean textual merge does not establish TOTK compatibility.
