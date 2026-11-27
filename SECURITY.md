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
| Typing into the focused window | `SendInput` (Unicode keystrokes, and scancode keys — Backspace, Shift, arrows, Ctrl+V — for the punctuation rewrite) | Only while the user dictates or the polish rewrite runs; text goes to the window the user clicked into. | The app re-checks foreground on every keystroke and stops if focus changes. Windows' own UIPI blocks injection into elevated windows — Tiro cannot type into an admin window. |
| Reading and restoring the clipboard | `OpenClipboard` / `GetClipboardData` / `SetClipboardData` | Only during the punctuation rewrite (a few hundred ms at the end of a dictation), never while idle or listening. | The rewrite borrows the clipboard to paste the polished text over the dictated span. Every existing clipboard format (each ≤ 64 MiB) is snapshotted first and restored immediately afterwards. Nothing is sent anywhere — the clipboard never leaves the machine. |
| Global hotkey | `RegisterHotKey` (default Ctrl+Alt+Space) | Always (single toggle key). | Standard shortcut API; no keyboard hooks. |
| Status overlay window | Layered, topmost, click-through, per-pixel alpha | Only while dictation is on; can be permanently disabled via tray menu ("Disable Eyeball Animation"). | Draws an animation reflecting microphone loudness. No screen content is read or captured — the window is output-only. |
| Message popups | Ordinary non-activating popup | Transient, on errors/instructions | Auto-dismiss after 5 s. No hooks. |
| Settings | Registry `HKCU\Software\Tiro` | On change / on use | Per-user only. No HKLM writes at all — the sole other registry touch is a read of the user's own theme key. |
| Autostart (opt-in) | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Tiro.lnk` | Only when the user checks "Enable at Startup" in the tray menu (unchecked removes it) | A plain shell shortcut to Tiro.exe created via IShellLink COM. No registry Run key, no scripts, no scheduled task, no service. Same user scope; removable by the user or by unchecking the menu item. |
| Files written | Log `AppData\Local\Tiro\logs\app.log`; optional model files `AppData\Local\Tiro\models` | Continuous (log) | No document, clipboard or file-system scanning. |

## What Tiro does NOT do

- No network I/O of any kind (no sockets, no HTTP, no telemetry, no update
  check). Updates are manual: download a new release.
- No keyboard/mouse hooks (`SetWindowsHookEx` is not used anywhere in the
  binary). *Removed in the 2026-11-26 security pass; earlier builds had a
  short-lived low-level keyboard hook used solely to let Escape close an
  error popup.*
- No code injection, no remote threads, no DLL loading from user-writable
  paths. The executable is self-contained (statically linked, zero
  side-loaded DLLs) and loads only Windows system DLLs.
- No persistence beyond its `HKCU` key (opt-in autostart aside).
- No screenshot or clipboard access.

## Debug recorder (off by default)

`HKCU\Software\Tiro\RecordWav` (DWORD) must be explicitly set to `1` to write
microphone WAVs for development diagnostics. **Release builds default to off**
and the key is absent on end-user machines.

## Embedded third-party components (statically linked; no DLLs shipped)

| Component | Version | Licence |
|---|---|---|
| sherpa-onnx (speech recognition engine) | 2.0.3 | Apache-2.0 |
| ONNX Runtime (inference) | 1.28.2 (as shipped inside the sherpa-onnx static package) | MIT |
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
facts above are intended to support an allow-list decision. Recommended
exception scope: file-path or publisher-based exception for `Tiro.exe`; no
network exception is needed because it never uses the network.

Releases are published with SHA-256 checksums next to the download; verify
the checksum of the binary your scanner flagged against the release page
before investigating further.
