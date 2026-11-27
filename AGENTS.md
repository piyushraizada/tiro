# AGENTS.md - working on Tiro with a coding agent

Tiro is a single-file Windows dictation app: a global hotkey starts WASAPI
capture, sherpa-onnx runs Nemotron streaming ASR **on device**, and the
transcript is injected into whatever control has focus. C++17, Win32, no
runtime dependencies, no network, no installer.

This file is the operational contract for automated agents. `BUILD.md` is the
human build guide, `dist/README.txt` is the user manual, and
`requirements_and_bdd.md` is the specification - where this file and the spec
disagree, the spec wins and this file needs fixing.

## Build

```bat
cmd.exe /c build_release.bat
```

- Run it from Windows (`cmd`), not from bash. From git-bash use:
  `MSYS2_ARG_CONV_EXCL='*' cmd.exe /c build_release.bat`
- It locates MSVC through `vswhere`, so no "Developer Command Prompt" is
  required. Visual Studio with **Desktop development with C++** must be
  installed.
- Success prints `stamp: <hash>` and `built: bin\Release\Tiro.exe`; the very
  first build also prints `libs: unpacking third_party\...tar.xz` while the
  bundled SDK unpacks.
- Every command in this file assumes the **repository root** as the working
  directory; the paths are relative.
- When calling `cmd.exe` from bash, keep `MSYS2_ARG_CONV_EXCL='*'` in front
  whenever the command contains `/` switches (`/IM`, `/AL`, `/C:`), or the
  shell mangles them.
- Output lands in `bin\Release`: `Tiro.exe`, `LICENSE.txt`, `NOTICE.txt`,
  `README.txt`, `SECURITY.md` and a `models` junction to `models\`. The junction
  is created only when `bin\Release\models` does not exist yet; if a real folder
  is already there, the build prints a note, and a model added under `models\`
  will not be visible beside the exe until it is copied in as well.
- The 14 speech-engine libraries are unpacked automatically from
  `third_party\sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.xz`
  using **Windows' own** `%SystemRoot%\System32\tar.exe`. Never replace that
  with a bare `tar`: Git/Cygwin/MSYS tars cannot read `.tar.xz` and fail with a
  misleading "Access is denied".
- `LNK1104: cannot open file '...\bin\Release\Tiro.exe'` means the app is
  running: `MSYS2_ARG_CONV_EXCL='*' taskkill /IM Tiro.exe /F`

## Speech model

The weights are **not** in the repository and the build does not fetch them. To
make a fresh clone dictate, follow `BUILD.md` "Step 3 - Get the speech model":
download the Nemotron streaming 0.6B `tar.bz2`, extract it into `models\`, and
name the folder `nemotron-0.6b-en`.

- Without it the build still succeeds and the app still starts, but the engine
  fails to load: the icon stays on the pale loading arc and a notification points
  at the log.
- `--debug --selftest` needs it too - it is the model under test, not a fixture.
- The optional punctuation model is a separate folder (`models\punctuation`) and
  is not needed for the self-test or for dictation.

## Verify - the only accepted proof

```bat
cmd.exe /c "bin\Release\Tiro.exe --debug --selftest test\fixtures\punct_selftest.wav"
```

The run is **silent** by design - a GUI binary writes only to the log - and it
takes about five seconds (engine start plus decoding), so do not kill it early.
Exit code 0 alone proves nothing, and app.log is shared by every build and
session on the machine, so prove the line came from this build:

```bat
cmd.exe /c findstr /N /C:"startup: Tiro 1.0.0" /C:"selftest final" "%LOCALAPPDATA%\Tiro\logs\app.log"
```

`/N` numbers the lines, so ordering is visible: take the **last**
`startup: Tiro 1.0.0 (<hash>)` line whose hash equals the build's `stamp:` (or
`git rev-parse --short HEAD`); a `selftest final:` line must appear after it.
Not necessarily immediately - debug mode logs the banner, core pinning,
`engine ready` and `selftest: 6 s of audio` in between.

Two hazards this rule exists to defeat:

- the log accumulates across builds **and** sessions; and
- rebuilding the same commit produces an **identical** hash, so an older
  `startup:` line can match your stamp exactly. Only the last such line counts.

`[WARN] hotkey registration failed at startup` is expected under `--debug`: the
diagnostic run does not own the hotkey (a live tray copy may hold it) and it
does not matter for the self-test.

Expected: `selftest final: Hello there. Who are you? I like tea but not coffee`
Word accuracy is what matters; a missing final period is native model
behaviour and is accepted.

The self-test needs no microphone and no audio device, and `--debug` lifts the
single-instance check, so it can run while a tray copy is live. There is no
unit-test harness; do not invent one.

## Hard rules

- **Windows x64 only.** Win32 + WASAPI + MSVC. Keep the static `/MT` runtime;
  the toolchain is pinned to `v145` (Visual Studio 2026 / VS 18).
- **On-device only.** No network calls, no telemetry, no external ASR or
  rewriting services. Everything the app needs ships beside the exe.
- **No installer, no elevation.** The shipped app writes only to HKCU and
  `%LOCALAPPDATA%\Tiro`; add no services, drivers, scheduled tasks or machine
  -wide registry keys.
- **Do not commit** `models/`, the unpacked `third_party/*/` folder, `bin/`,
  `build/` or `src/gitver.h`. The 55 MB `.tar.xz` archive and the two C API
  headers **are** tracked on purpose.
- **Do not re-add punctuation processing.** Rev 31 removed the self-written
  middleware; the app injects raw model output. The optional polish model is
  opt-in and off by default - leave it optional.
- **Behavioural code changes sync the spec**: update
  `requirements_and_bdd.md` (new revision entry plus the affected BDD
  scenarios) in the *same* commit.
- **Documentation must match the binary.** If you change the tray states,
  notifications, hotkeys or diagnostics, update `dist/README.txt`,
  `dist/NOTICE.txt`/`LICENSE.txt` as needed and `BUILD.md` - the About box and
  the manual are user-facing claims, not aspiration.
- **Third-party licences stay complete.** sherpa-onnx is Apache-2.0, ONNX
  Runtime is MIT, the model is under the NVIDIA Open Model License. If you
  bump the SDK, replace the archive, update `third_party\licenses\README.txt`
  and `dist\LICENSE.txt`/`NOTICE.txt` in the same change.

## Release artifact

Only at release time, from a clean committed tree:

```bat
cmd.exe /c make_release.bat 1.0.0
```

It refuses a dirty tree, tags `v<version>`, stages `Tiro\`, writes
`build\Tiro-1.0.0-win64.zip`, re-opens the zip to verify the entry list and the
exe hash, and writes a `.sha256` beside it. Builds are **not** zipped; never
hand-zip a release.

## Where things are

| Path | What |
|---|---|
| `src/main.cpp` | The application: hotkey, capture, engine, injection, tray, dialogs, diagnostics |
| `src/punctuation.*` | Optional polish model plumbing (off by default) |
| `src/Tiro.rc`, `src/resource.h`, `res/*.ico` | Version resource and the eye tray art (green = listening, red = ready, pale arc = starting) |
| `Tiro.vcxproj`, `Tiro.sln` | Build definition; toolset and library paths live here |
| `build_release.bat` | The build |
| `make_release.bat` | The gated release zip |
| `dist/` | Documents shipped beside the exe, plus per-model licence files |
| `third_party/include`, `third_party/licenses` | Tracked C API headers and licence texts |
| `test/fixtures/punct_selftest.wav` | The offline proof fixture (forced into Git; `*.wav` is ignored) |
| `PROVENANCE.md` | Where every file comes from and under what licence it ships; a new file needs a row in the same commit |
| `requirements_and_bdd.md` | Specification and BDD scenarios - the source of truth |
