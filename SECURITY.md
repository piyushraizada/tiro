# Tiro — security statement for IT review

Tiro is a local, offline speech-to-text (dictation) utility for Windows. All
transcription runs in-process with ONNX Runtime; **the application opens no
network connections at all**. This document lists every sensitive API surface
so an EDR/AV exception can be granted on facts, and states which third-party
components are embedded and in which version.

Publisher: Piyush Raizada · License: see NOTICE.txt

## Runtime behaviour (what the binary does with sensitive APIs)

| Behaviour | API | When | Notes |
|---|---|---|---|
| Microphone capture | WASAPI `IAudioCaptureClient`, 16 kHz mono | **Only while dictation is on** (user hotkey). Capture is released when off. | No other audio device access. No recording unless explicitly enabled by registry (see Debug recorder). |
| Typing into the focused window | `SendInput` (Unicode keystrokes) | Only while the user dictates, or when the drained tail is typed at stop; text goes to the window the user clicked into. | The target is pinned when dictation turns on and Tiro checks it is still the foreground window before typing **each segment**; characters already being typed within one segment cannot be recalled. Windows' own UIPI blocks injection into elevated windows — Tiro cannot type into an admin window. |
| Reading Ctrl/Alt/Shift/Win state | `GetAsyncKeyState` | Momentarily, before typing text that follows a hotkey press | Modifier state only — never characters, no key history, nothing while idle. It exists so text typed just after Ctrl+Alt+Space is not reinterpreted as a shortcut. |
| Reading and updating the document | `EM_GETSEL` / `EM_GETTEXTRANGE` / `EM_EXSETSEL` / `EM_REPLACESEL` (classic EDIT, RichEdit) and UI Automation `TextPattern` / `ValuePattern` | Only during the optional punctuation rewrite, on the window the user has just dictated into | The rewrite **reads the span back** before replacing it and aborts unless it is exactly the text Tiro typed. Nothing is sent anywhere. |
| Clipboard | none, except the About dialog | About dialog → **Copy details** button, on explicit user click | That button writes diagnostic text and replaces whatever the clipboard held, exactly like any app's Copy button. Tiro never *reads* the clipboard, and the dictation and rewrite paths never touch it. |
| Global hotkey | `RegisterHotKey` (default Ctrl+Alt+Space) | Always (single toggle key). | Standard shortcut API; no keyboard hooks. |
| Status overlay window | Layered, topmost, click-through, per-pixel alpha (`WS_EX_LAYERED \| WS_EX_TRANSPARENT`) | Only while dictation is on; can be permanently disabled via tray menu ("Disable Eyeball Animation"). | Draws an animation reflecting microphone loudness. No screen content is read or captured — the window is output-only, and mouse input passes through to whatever is underneath. |
| Message popups | Non-activating, click-through popup (`WS_EX_NOACTIVATE \| WS_EX_TRANSPARENT`) | Transient, on errors/instructions | Auto-dismiss after 5 s. No hooks, no swallowed clicks. |
| Settings | Registry `HKCU\Software\Tiro` | On change / on use | Per-user only. No HKLM writes at all — the sole other registry touch is a read of the user's own theme key. Every numeric setting is range-checked before use. |
| Model files | `CreateFileW` on `models\` beside the executable, read-only | At engine load | Nothing is written, copied or cached. Expected SHA-256 values ship inside the model folder as `SHA256SUMS.txt` and are enforced by the release script. |
| Autostart (opt-in) | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Tiro.lnk` | Only when the user checks "Enable at Startup" in the tray menu (unchecked removes it) | A plain shell shortcut to Tiro.exe created via IShellLink COM. No registry Run key, no scripts, no scheduled task, no service. Same user scope; removable by the user or by unchecking the menu item. |
| Files written | Log `AppData\Local\Tiro\logs\app.log`; optional model files `AppData\Local\Tiro\models` | Continuous (log) | No document, clipboard or file-system scanning. A message longer than the log line buffer is truncated, never a crash. |

## What Tiro does NOT do

- No network I/O of any kind (no sockets, no HTTP, no telemetry, no update
  check). The release binary imports **no networking library at all** — verified
  with `dumpbin /dependents`. Updates are manual: download a new release.
- No keyboard/mouse hooks (`SetWindowsHookEx` is not used anywhere in the
  binary). *Removed in the 2026-11-26 security pass; earlier builds had a
  short-lived low-level keyboard hook used solely to let Escape close an
  error popup.*
- No keystroke or clipboard **capture**. Tiro is an output device: it types, it
  never records what the user types.
- No code injection, no remote threads, no DLL loading from user-writable
  paths. The executable is self-contained (statically linked, zero side-loaded
  DLLs) and loads only Windows system DLLs; the one runtime `LoadLibrary` call
  is pinned to System32.
- No persistence beyond its `HKCU` key (opt-in autostart aside).
- No screenshot or screen reading.

## Debug recorder and diagnostic switches (off by default)

`HKCU\Software\Tiro\RecordWav` (DWORD) must be explicitly set to `1` to write
microphone WAVs for development diagnostics. **Release builds default to off**
and the key is absent on end-user machines. `LogText` (default `0`) additionally
writes the dictated words to the log.

The diagnostic switches (`--about`, `--hotkey`, `--notify`, `--startup-toggle`,
`--restart`, `--selftest <wav>`, `--live <wav>`, `--polish-file`,
`--polish-tokens`, `--rewrite-demo`, `--probe-focus`) are **inert unless
`--debug` is also on the command line**, and they grant no privilege: they read
and write only the file paths they are given. A normal launch of `Tiro.exe`
never sets `--debug`.

## Build hardening

The release binary is built with Control Flow Guard (`/guard:cf` — every indirect
call is validated against a bitmap), ASLR with high entropy (`/DYNAMICBASE`,
`/HIGHENTROPYVA`), NX (`/NXCOMPAT`) and stack cookies (`/GS`). Its embedded
manifest requests `asInvoker`: launching it never prompts for elevation and it
holds no administrator rights.

## Integrity

- The executable is **unsigned by design**. There is therefore **no
  publisher-based exception to offer** — the correct rule is a file-path or
  SHA-256 exception for `Tiro.exe`.
- Every release publishes a SHA-256 for the zip beside the download, and the
  release script re-hashes the packaged exe against the build output before the
  zip is written.
- The speech model is pinned: `SHA256SUMS.txt` in `models\nemotron-0.6b-en\`
  lists the SHA-256 of every model file, the release script refuses to package a
  model that has changed without a deliberate manifest update, and anyone can
  re-check it with `certutil -hashfile encoder.int8.onnx SHA256`.

## Embedded third-party components (statically linked; no DLLs shipped)

| Component | Version | Licence |
|---|---|---|
| sherpa-onnx (speech recognition engine) | 2.0.3 | Apache-2.0 |
| ONNX Runtime (inference) | 1.28.2 runtime, C API level 20 (as shipped inside the sherpa-onnx static package) | MIT |
| NVIDIA Nemotron streaming ASR model (`nemotron-speech-streaming-en-0.6b`) | 560 ms int8 ONNX export | NVIDIA Open Model License |
| Horizon-Labs `punctuation-restoration-base` model | current | Apache-2.0 (fine-tune of mmBERT-base, MIT) |
| Microsoft Visual C++ runtime (/MT) | VS 2022+ toolset | VS redistributable licence terms |

Full licence texts ship in `LICENSE.txt` and `NOTICE.txt`. Because everything
is statically linked there are no third-party DLL files on disk; the version
numbers above are the authoritative inventory (our SBOM). When an upstream
security fix lands, we rebase and publish a new release — the release notes
of each version record the component versions it carries.

## If your scanner flags Tiro

Dictation software legitimately combines microphone capture and keystroke
injection, which overlaps with generic spyware heuristics. The behavioural
facts above are intended to support an allow-list decision. Two calls may draw
attention and both are benign: `SendInput` (Tiro types into the window the user
chose — it cannot read the keyboard) and `GetAsyncKeyState` on the four modifier
keys only (used so typed text is not interpreted as a shortcut). Recommended
exception scope: a **file-path or SHA-256 rule** for `Tiro.exe`; no network
exception is needed because it never uses the network.

Releases are published with SHA-256 checksums next to the download; verify
the checksum of the binary your scanner flagged against the release page
before investigating further.
