# Building Tiro from source

Tiro is a Win32 C++ application built with MSBuild. There is no CMake, no
vcpkg, no Python and no installer project: one `.vcxproj`, one batch file.

This page takes you from a fresh clone to a running app. Budget about twenty
minutes the first time (most of it downloads) and one to two minutes per build
after that.

**You do not need a Developer Command Prompt or any special shell.**
`build_release.bat` locates the MSVC environment itself with `vswhere`, loads
it, stamps the commit, builds, and copies the end-user documents into the
output folder.

---

## What you need

| Requirement | Notes |
|---|---|
| Windows 10 or 11, 64-bit | The app is x64 only. |
| Visual Studio with **Desktop development with C++** | Visual Studio 2026 / VS 18 ships the `v145` toolset the project asks for. Visual Studio 2022 works if you retarget (step 2). The free Community edition is enough. |
| Windows SDK 10.0.26100 | Installed by that workload. Any newer SDK works if you retarget. |
| Git | Used only to stamp the commit hash into the About box. Without it the build still succeeds and the stamp reads `unknown`. |
| ~2.5 GB free disk | 1.1 GB of speech-engine libraries (unpacked by the build, once) + 464 MB model, plus `bin\Release`. |
| A microphone | Only to use the app. The build and the offline self-test need none. |

---

## Step 1 - Get the source

```bat
git clone https://github.com/piyushraizada/tiro.git
cd tiro
```

No Git? Use **Code -> Download ZIP** on the repository page and unpack it; the
build will stamp `unknown` instead of a commit hash.

---

## Step 2 - Install the C++ toolchain

1. Run the Visual Studio Installer, choose **Desktop development with C++**,
   and make sure these individual components are ticked:
   - **MSVC v145 - VS 2026 C++ x64/x86 build tools** (or the v143 set for VS 2022)
   - **Windows 11 SDK (10.0.26100)**
2. Nothing else: no MFC, no ATL, no C++/CLI, no CMake component.

**Using Visual Studio 2022?** The project pins `PlatformToolset = v145` and
`WindowsTargetPlatformVersion = 10.0.26100.0`. Two ways to proceed:

- Open `Tiro.vcxproj` in Visual Studio, accept the **Retarget Projects**
  prompt, and save; or
- leave the project alone and pass the toolset on the command line:
  `msbuild Tiro.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143`

The rest of this page assumes `build_release.bat`, which uses the project's
own settings.

---

## Step 3 - Get the speech model

The model is 464 MB and is not in the repository (model weights do not belong
in Git).

1. On the same releases page, open the **asr-models** release
   (<https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models>).
2. Download
   **`sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25.tar.bz2`**
   (about 464 MB).
3. Extract it into `models\` and rename the folder to `nemotron-0.6b-en`:

```bat
mkdir models
tar -xjf "%USERPROFILE%\Downloads\sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25.tar.bz2" -C models
ren models\sherpa-onnx-nemotron-speech-streaming-en-0.6b-560ms-int8-2026-04-25 nemotron-0.6b-en
```

4. The tarball carries the weights and `tokens.txt` but **not** `model.json` -
   that file is Tiro's own manifest. Create
   `models\nemotron-0.6b-en\model.json` with exactly this content:

```json
{
  "name": "Nemotron 0.6B (English, 560 ms)",
  "family": "transducer",
  "files": {
    "encoder": "encoder.int8.onnx",
    "decoder": "decoder.int8.onnx",
    "joiner": "joiner.int8.onnx",
    "tokens": "tokens.txt"
  },
  "sample_rate": 16000,
  "language": "en",
  "chunk_ms": 560,
  "license": "NVIDIA Open Model License",
  "notice": "Licensed by NVIDIA Corporation under the NVIDIA Open Model License",
  "source": "https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b"
}
```

The folder must end up with five files:

```bat
dir models\nemotron-0.6b-en
```

```
decoder.int8.onnx      encoder.int8.onnx      joiner.int8.onnx
model.json             tokens.txt
```

The folder name itself is free - Tiro lists every `models\<folder>` that
contains a valid `model.json` - but the rest of these instructions assume
`nemotron-0.6b-en`, and the build warns when it is absent.

Keep the model in the repository's `models\` folder. Tiro reads
`<exe folder>\models`, and the build creates `bin\Release\models` as a
directory junction to the repository's `models\` - so there is one 633 MB copy
on disk, not two, and `bin\Release\Tiro.exe` finds it. If you later copy
`bin\Release` somewhere else, the copy follows the junction and carries the
weights with it.

---

## Step 4 - Build

From the repository root, in any Command Prompt (or just double-click the
file in Explorer):

```bat
build_release.bat
```

What it does, in order: finds `vcvars64.bat` through `vswhere`, loads the MSVC
environment, writes the commit hash into the generated `src\gitver.h`, runs
`msbuild` for Release x64, copies `README.txt`, `LICENSE.txt`,
`NOTICE.txt` and `SECURITY.md` next to the executable, and links
`bin\Release\models` to the repository's `models\` folder. The first build
takes a couple of minutes; later ones are quicker.

Successful output looks like this (`<hash>` is your commit):

```text
toolchain: C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
stamp: <hash>
  Tiro.vcxproj -> D:\...\bin\Release\Tiro.exe
models: linked bin\Release\models -> D:\...\models

built: bin\Release\Tiro.exe
```

If it stops with `error: vcvars64.bat was not found`, the C++ workload is not
installed (step 2); you can also open **x64 Native Tools Command Prompt for
VS** and run `msbuild Tiro.vcxproj /p:Configuration=Release /p:Platform=x64`
by hand.

---

## How to tell the build actually worked

Five checks, about a minute. Each one is independent proof of a different
layer.

1. **The build said so.** The console ends with `stamp: <hash>`,
   `Tiro.vcxproj -> ...\bin\Release\Tiro.exe` and `built: bin\Release\Tiro.exe`,
   with no `error C....` or `error LNK....` lines. The script exits 0 on
   success (`echo %ERRORLEVEL%` in cmd, `echo $?` in git-bash). On the **first**
   build only, a line `libs: unpacking third_party\...tar.xz (about a minute)`
   appears before MSBuild starts - the bundled SDK is unpacking itself.

2. **The output folder is complete.**

   ```bat
   dir bin\Release
   ```

   must show `Tiro.exe` (about 19 MB), `README.txt`, `LICENSE.txt`,
   `NOTICE.txt`, `SECURITY.md` and `models`. That `models` entry is a junction
   (`dir /AL bin\Release` marks it `<JUNCTION>`), so
   `bin\Release\models\nemotron-0.6b-en\model.json` must resolve.

3. **The binary runs and reports its identity.** Double-click
   `bin\Release\Tiro.exe`. An eye icon appears in the notification area: a pale
   grey arc circles it while it loads for a few seconds, then a red glow along
   its lower edge means ready (that glow turns green while dictation is on).
   Right-click it, choose **About**, and it reads
   `Tiro 1.1.0 (<hash>)` - the same hash as the `stamp:` line. That is how you
   know you are running the code you just compiled. Headless equivalent, if you
   cannot use the tray:

   ```bat
   powershell -NoProfile -c "(Get-Item 'bin\Release\Tiro.exe').VersionInfo.FileVersion"
   ```

   prints `1.1.0.0`; the commit hash itself is in that build's `src\gitver.h`
   and in the log's `startup:` line.

4. **The engine loaded.** Open `%LOCALAPPDATA%\Tiro\logs\app.log`. It should
   contain

   ```text
   startup: Tiro 1.1.0 (<hash>)
   engine ready in .... ms (threads=1)
   ```

   `engine ready` means the model and the libraries are wired up correctly; a
   missing or malformed model shows `recogniser creation failed` and a
   "Transcription engine failed to load" pop-up instead.

5. **No microphone needed: replay the bundled test clip.** Run this from the
   repository root:

   ```bat
   bin\Release\Tiro.exe --debug --selftest test\fixtures\punct_selftest.wav
   ```

   The command prints **nothing** - Tiro is a GUI binary, so every line goes to
   the log - and it takes about five seconds (engine start plus decoding the
   clip), so give it a moment rather than killing it. app.log then gains

   ```text
   selftest final: Hello there. Who are you? I like tea but not coffee
   ```

   app.log is shared by every build and session on the machine, so "the log
   contains that line" is not proof by itself - and rebuilding the same commit
   yields the same hash, so an older `startup:` line can match your stamp too.
   Prove it was this run: take the **last** `startup: Tiro 1.1.0 (<hash>)` line
   whose hash equals the build's `stamp:`, and require a `selftest final` line
   after it (debug mode logs the banner, core pinning, `engine ready` and
   `selftest: 6 s of audio` in between, so not immediately after).

   ```bat
   findstr /N /C:"startup: Tiro 1.1.0" /C:"selftest final" "%LOCALAPPDATA%\Tiro\logs\app.log"
   ```

   The line numbers make the order visible: the last `startup:` line carrying
   your hash must be followed by your `selftest final` line.

   Under `--debug` the log also shows `[WARN] hotkey registration failed at
   startup`. That is expected - the diagnostic run does not register the
   hotkey - and it does not affect the self-test.

   Different wording is fine (the model is what it is); what matters is that a
   transcript appears at all. This runs the real decoder against a real audio
   file without touching your microphone. `--debug` is required - the
   diagnostic switches are inert in a normal launch.

Then click the tray icon once and speak: your words appear at the cursor.

---

## Where the application is

`bin\Release\` **is** the application. It is portable: copy that folder
anywhere - Desktop, USB stick, another PC - and double-click `Tiro.exe` there.
Keep `models\` beside the executable; that is where Tiro looks for the speech
model.

Files Tiro creates at runtime live in your own profile, never in that folder:

| Path | What |
|---|---|
| `HKCU\Software\Tiro` | microphone, model and hotkey choices |
| `%LOCALAPPDATA%\Tiro\logs\app.log` | event log (no dictation text by default) |
| `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Tiro.lnk` | only if you enable autostart |

`bin\Release` is unsigned, so the first launch of your own build may show a
SmartScreen prompt: **More info -> Run anyway**.

The `build\` folder is scratch. A distributable zip is only produced by
`make_release.bat <version>`, which additionally refuses to run on an
uncommitted tree, checks the model files against `dist\models.sha256` (the
pinned hashes of the weights that ship), and verifies the archive it wrote.

---

## What is deliberately not in the repository

| Not in Git | Why | Where it comes from |
|---|---|---|
| `models\` (464 MB download, ~632 MB unpacked) | Model weights are data, not source | Step 3 |
| `third_party\*.lib` (1.1 GB unpacked) | Unpacked on the first build from `third_party\sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.xz`, which **is** in the repo | `build_release.bat` |
| `bin\`, `build\` | Build output | Produced by the build |
| `src\gitver.h` | Generated on every build | `build_release.bat` |

Both third-party projects are permissive (Apache-2.0 and MIT), so their
material may be redistributed as long as the notices travel with it. The two
C API headers are tracked in `third_party\include`; the 14 static libraries are
tracked as **one xz-compressed tarball (55 MB)** - uncompressed they are 1.1 GB
and Git refuses any single file over 100 MB, and xz packs them less than half
as large as the upstream .tar.bz2. The build unpacks that tarball into
`third_party\` on its first run, so there is no separate SDK download.
`third_party\licenses\README.txt` records the provenance and licence position.

---

## Troubleshooting

| Message | Meaning | Fix |
|---|---|---|
| `error: vcvars64.bat was not found` | No Visual Studio C++ toolset | Install **Desktop development with C++**, or run the build from *x64 Native Tools Command Prompt for VS* |
| `MSB8020: The build tools for v145 cannot be found` | Older Visual Studio | Retarget the project (step 2) or pass `/p:PlatformToolset=v143` |
| `MSB8036: The Windows SDK version 10.0.26100.0 was not found` | SDK mismatch | Install that SDK, or set `WindowsTargetPlatformVersion` to one you have |
| `C1083: cannot open include file: 'sherpa-onnx-c-api.h'` or `'onnxruntime_c_api.h'` | The clone is incomplete - both headers ship with the repository | Check out the full tree, or restore `third_party\include` from Git |
| `LNK1104: cannot open file '...\bin\Release\Tiro.exe'` | Tiro is running and holds the exe | Exit Tiro from the tray, then build |
| `error: ...\tar.exe was not found` | Windows older than 10 1803, which shipped `tar.exe` | Build on Windows 10 1803+; the build always calls `%SystemRoot%\System32\tar.exe` by full path, so an MSYS/Cygwin tar on PATH is never used |
| `error: could not unpack the speech-engine libraries (tar exit N)` | The tarball is damaged, or an antivirus blocked extraction | Re-check out the tarball (`git checkout -- third_party`), then rebuild |
| `LNK2019: unresolved external symbol` | The unpacked libraries are missing or incomplete | Restore `third_party\sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.xz` (it ships with the repo), delete any partial `...-lib` folder, rebuild |
| `warning: could not create the bin\Release\models link` | Filesystem or policy refused the junction | Copy the repository's `models` folder to `bin\Release\models` by hand (633 MB) |
| `stamp: unknown` | Git not found, or not a git checkout | Cosmetic only; install Git and rebuild for a real hash |
| App starts, tray stays on the pale loading arc, "Transcription engine failed to load" | Model missing or malformed | Check `models\nemotron-0.6b-en` has all five files and a valid `model.json` |
| Tray menu lists `(no models found)` | Same as above | Same as above |
| `--selftest` says "expected 16 kHz mono 16-bit WAV" | Wrong audio format | Any 16 kHz mono 16-bit PCM WAV; the fixture in `test\fixtures\` is one |

Debug builds are not supported by this checkout: the SDK package carries
Release `/MT` libraries only, so build Release (which the script does).
