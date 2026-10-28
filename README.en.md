# Animal Crossing: New Horizons for Apple Silicon

[简体中文](README.md) | English

This is a personal experiment to run Nintendo Switch version 3.0.0 of *Animal Crossing: New Horizons* on Apple Silicon Macs and iPhones. The current goal is offline, single-player use. This is not a complete emulator, and the repository does not include game files, firmware, or encryption keys.

## Why I started this project

I came across [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and wondered whether other Switch games could be ported too. My child loves *Animal Crossing: New Horizons*, so I decided to give it a try.

During development, I learned from and reused engineering work from [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [MacNativeGames](https://github.com/ly05010419/MacNativeGames). Thank you to both projects' developers and contributors for sharing their research and code. The reused components, their sources, and their licenses are listed in [NOTICE.md](NOTICE.md).

## Current status

- Target game version: ACNH 3.0.0. Experimental build paths exist for Apple Silicon Macs and iPhones.
- macOS: An offline prototype and repeatable build and module-check workflows are available. Some game services and graphics behavior are still incomplete.
- iPhone: An app package can be built, but input, save recovery, background and foreground behavior, and long-session stability have not all been verified.
- Online play: Nintendo Online, friend services, and local wireless play are not supported.

## Before you start

### 1. Computer and development tools

You need an Apple Silicon Mac running macOS 26 or later, plus the full Xcode 26 or later Metal toolchain. Xcode Command Line Tools alone are not enough. Install Xcode from [Apple's Xcode page](https://developer.apple.com/xcode/), open it once, and accept its license. Then confirm that the command line can find Xcode:

```fish
xcode-select -p
xcrun --find metal
```

Install [Homebrew](https://brew.sh/), then install the Mac build dependencies:

```fish
brew install uv llvm dotnet spirv-cross opus libvpx openssl@3 brotli
```

The `dotnet` formula provides the .NET 10 SDK. The shader translator downloads .NET dependencies the first time it is built. `opus` and `libvpx` provide audio and video support.

To build an iPhone package, install these additional tools:

```fish
brew install cmake xcodegen
```

Signing and installing on a real iPhone also requires an Apple developer team signed in to Xcode and a valid device signing setup. An unsigned IPA is only for local build and resource checks; it cannot be installed directly.

### 2. Game input files

Provide your own legally obtained files and keep them on your computer. By default, place them in the repository root, next to `build.py`:

```text
animal-crossing/
├── Animal Crossing New Horizons.xci
├── Animal Crossing New Horizons v3.0.0.nsp
├── prod.keys
└── title.keys
```

The XCI should contain the base game, and the NSP should be the version 3.0.0 update. Common plain-text key files are supported. If your files have different names or locations, pass `--base`, `--update`, and one or more `--keys` options. For example:

```fish
.venv/bin/python build.py \
  --base "/path/on/this-mac/Animal Crossing New Horizons.xci" \
  --update "/path/on/this-mac/Animal Crossing New Horizons v3.0.0.nsp" \
  --keys "/path/on/this-mac/prod.keys" \
  --keys "/path/on/this-mac/title.keys" \
  prepare
```

Do not commit game files, keys, extracted resources, or saves to GitHub. Do not paste or upload them to chat tools.

### 3. Download the project and install Python dependencies

Download the source archive from GitHub, or clone the repository, then open a terminal in the project root:

```fish
git clone https://github.com/ZerrionGao/animal-crossing.git
cd animal-crossing
```

Install the Python dependencies:

```fish
uv venv .venv --python 3.11
uv pip install --python .venv/bin/python -r requirements.txt
```

This creates a Python environment inside the project and does not change the system Python installation.

## Build the macOS prototype

Run these commands in order from the project root. Continue only when each command succeeds. If one fails, keep the error output and investigate it before continuing; do not skip checks and start the game anyway.

```fish
.venv/bin/python build.py prepare
.venv/bin/python build.py rewrite
.venv/bin/python build.py --jobs 2 verify
make -C games/acnh analyze
.venv/bin/python build.py runtime
.venv/bin/python build.py probe
```

These steps extract and check game resources, rewrite three game modules, run structural and differential checks, inspect startup and constructor code, compile the Mac runtime, and run a loader probe. **Passing these checks only means these build gates passed. It does not mean the game is fully playable.**

To package a Mac app, you also need NgWord2 data from your own XCI. Extract it into the local work directory:

```fish
.venv/bin/python games/acnh/scripts/ngc_assets.py \
  --source "Animal Crossing New Horizons.xci" \
  --keys prod.keys \
  --keys title.keys \
  --output work/acnh/3.0.0/ngc
.venv/bin/python build.py package
```

The app is created at `work/acnh/3.0.0/Animal Crossing New Horizons.app`. Build products, extracted files, logs, and shader caches stay in the Git-ignored `work/` directory. The `package` command creates the app but does not launch it.

The `boot` command launches the local prototype and accesses game resources. Use it for diagnosis only after the checks above pass. It may stop when it reaches an unsupported Switch service. Logs are saved under `work/acnh/3.0.0/`.

## Build an iPhone package

First complete the Python setup, game resource extraction, and NgWord2 preparation above. Then build an unsigned IPA for local preflight:

```fish
.venv/bin/python games/acnh/ios/tools/build_ipa.py --unsigned \
  --output work/acnh/3.0.0/ACNH-iPhone-unsigned.ipa
```

To sign and install on a real device, configure your Apple developer account and device in Xcode, then provide your signed-in team ID and connected device ID:

```fish
.venv/bin/python games/acnh/ios/tools/build_ipa.py \
  --team-id "replace with your Apple Development Team ID" \
  --device "replace with your connected device ID" \
  --output work/acnh/3.0.0/ACNH-iPhone.ipa
```

Do not send team credentials, private device information, or signing materials to an AI agent. A successful signature does not mean real-device behavior has been fully verified.

## Build with a local AI agent

If you use a local AI coding tool, give it the project directory and the following instructions. **XCI, NSP, keys, saves, and generated game resources must stay on your computer. Do not let the agent upload, share, or print their contents.**

```text
Read README.md and follow its "Build the macOS prototype" steps from the local project root.
First check whether the default input file paths exist. Do not manually open, print, or upload keys, game files, saves, or generated resources. The project commands may read them locally as part of the build.
Run the prepare, rewrite, verify, analyze, runtime, and probe commands in that section in order. Continue only after each command succeeds.
If any command fails, stop and report the failed command, a redacted error, and the log path. Do not skip a gate or edit source code to bypass a failure.
To build the .app, first confirm that local NgWord2 data is at work/acnh/3.0.0/ngc, then follow the README extraction and package commands.
Do not run boot unless I ask you to launch the game. Report each command as passed or failed, and state that passing build checks does not prove full playability or real-device acceptance.
```

If your game files use different names, give the agent their local paths so it can update the command. Do not give the files themselves to a cloud-hosted model.

## Known issues

- Only ACNH 3.0.0 is currently targeted and checked. Other game versions, Intel Macs, and other chip platforms are outside the current support scope.
- The Mac has displayed an airport scene and has a gameplay record. However, some system and graphics features are incomplete, and the overall acceptance checks are not finished. If the game calls an unsupported graphics function, it may stop.
- The project is offline-only. Network connections, friends, cloud saves, and local wireless play are not implemented.
- The iPhone prototype still needs checks for character and island name input, save read-back and recovery, background and foreground behavior, resource integrity, and long sessions.
- The iPhone can still get hot during long sessions. Current observations are not enough to quantify temperature or power improvements.
- NgWord2 preparation currently requires all 54 expected files. The XCI checked on the developer's Mac is missing `ac_similar_form_nx` and `table_similar_form_nx`, so strict extraction stops and packaging cannot finish with that input. Do not bypass this check with empty or placeholder files.
- The build requires a native toolchain and generates large resources and shader files from local game inputs. The first build downloads dependencies; build time and disk usage depend on your computer.

## Future work

The next steps are to fill in missing service and graphics behavior, investigate performance and heat, improve stability, and assess the limits of online support. Android and HarmonyOS are also areas of interest, but neither is implemented and there is no schedule yet. Each would require separate work on system services, GPU backends, signing, and performance.

## Thanks and contact

Thank you to the authors and contributors of [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [MacNativeGames](https://github.com/ly05010419/MacNativeGames), and to everyone who reads, tests, and shares feedback on open-source projects.

Contact me on Xiaohongshu: **Zerrion**.

The project's own code is licensed under the [MIT License](LICENSE). Original copyright and license information for reused components is in [NOTICE.md](NOTICE.md). Please make sure you have the right to use your local game inputs.
