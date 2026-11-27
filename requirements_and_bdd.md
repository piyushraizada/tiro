# Tiro — Real-Time Dictation for Windows

**Version 1.0.0 · Revision 1**

This is the specification for Tiro 1.0.0 as built and shipped. Every number and
behaviour below is taken from the implementation (`src/main.cpp`,
`src/punctuation.cpp`), and every requirement has at least one scenario in §7.

Where the product has a limitation, this document states the limitation as a
requirement rather than omitting it. There is no history here: no superseded
design, no revision log, no future intent dressed as present behaviour.

---

## 1. Overview

Tiro is a tray application that turns speech into text in whatever control has
focus. A global hotkey opens the microphone, a streaming speech model running on
the local CPU decodes it, and the transcript is typed into the target window as
Unicode input.

- **On device, always.** No network calls, no telemetry, no cloud service.
- **Portable.** One executable; models are folders beside it; nothing is
  installed and nothing requires elevation.
- **Quiet.** One tray icon, one pop-up at a time, and no window unless the user
  asks for one.

**Priorities, in order:** accurate words; a toggle that works first time; never
typing into the wrong window; punctuation last.

---

## 2. Scope

### 2.1 In scope

Tray-resident dictation in a single local desktop session: capture, streaming
recognition, Unicode injection, microphone and model selection, a configurable
hotkey, optional Startup registration, an optional punctuation rewrite, and the
diagnostics needed to support all of it.

### 2.2 Out of scope for 1.0.0

- Remote Desktop, session switching, resume-from-sleep recovery.
- Following the Windows default capture device *while* listening (§5.1.5).
- Voice commands, spoken punctuation, command grammars.
- A self-written punctuation middleware (the model's own punctuation is the
  product behaviour).
- Installers, services, drivers, scheduled tasks, machine-wide settings.
- Localization: the UI is English only, whatever language the model emits.

---

## 3. User interface

### 3.1 Tray icon

One icon, three states, each a distinct `.ico` loaded from the resources at
16 px (`SM_CXSMICON`).

| State | Asset | Tooltip | Meaning |
|---|---|---|---|
| Loading | `res/tiro_loading.ico` | `Tiro - starting` | Process up, engine not ready |
| Off | `res/tiro_off.ico` | `Tiro - off` | Engine resident, microphone closed |
| On | `res/tiro_on.ico` | `Tiro - listening` | Microphone open, decoding |

The artwork is the application's eye — a dark metal collar around a red-orange
lens:

- **Loading:** pale grey arcs circle the collar; no lens glow.
- **Off:** dark lens, a red band glowing along the lower rim.
- **On:** lit lens, a green band along the lower rim.

Band colour is the primary cue, band position the secondary, so "starting" can
never be mistaken for "off" at 16×16. Each size (16/24/32/48) is drawn for that
size, and each element carries a thin contrasting halo so the icon reads on
light and dark taskbars.

**State rules**

- The process starts in Loading and leaves it when the engine reports ready.
- If the engine fails at startup the state stays Loading for the life of that
  process: pale arc, microphone and model submenus greyed, and Exit is the only
  useful action. Recovery is a restart.
- Every stop path ends in Off: user stop, focus loss, device loss, engine
  failure after a successful start, opening a dialog, changing the model.
- The shell icon is added once and then modified. An Explorer restart re-adds
  the icon and re-registers the hotkey.

### 3.2 Pop-ups

Two classes, never visible together.

| | Eyeball status pop-up | Message notification |
|---|---|---|
| When | On turn-on, and whenever audio is heard while On | Any information, warning or error |
| Content | Animated eyeball, no text | Text only |
| Backdrop | Transparent, per-pixel alpha | Opaque panel in the current app theme |
| Size | 148 px square | Measured from the text + 28 px padding, 360×80 minimum |
| Position | Screen centre | Primary monitor work area, 16 px from right and bottom |
| Lifetime | While listening with audio | Exactly 5 s |

The eyeball animates on a 33 ms timer and opens with input level (gain 1600,
attack 750 ms, release 280 ms — registry-tunable). It confirms the
microphone is being heard; it says nothing about recognition.

A message replaces the eyeball at once, is topmost, never takes focus, and
cannot be clicked or hovered to stay. Its text is also written to the log at
`INFO`.

### 3.3 Tray menu

Rebuilt from live state on every right-click, so devices plugged in and model
folders copied while the app runs appear without a restart.

| # | Item | Behaviour |
|---|---|---|
| 1 | **Select Microphone ▸** | `(System default)` then each active capture device by friendly name (max 90), current choice checked. Greyed while Loading; `(no microphones)` when none. Choosing while Off stores it; while On reopens capture immediately and restores the previous device if the new one fails. |
| 2 | **Select Model ▸** | Usable models by manifest name (max 90), active one checked. Greyed while Loading; `(no models found)` when none. Choosing another ends dictation, unloads the engine, stores the choice; the model loads at the next turn-on. |
| 3 | *separator* | |
| 4 | **Configure Hotkey…** | Opens §3.5. |
| 5 | **Disable Eyeball Animation** | Checked when the eyeball is off; applies immediately, hiding a visible pop-up. |
| 6 | **Disable Punctuation** | Checked when the rewrite is off (default). Clicking only records intent for the next start, and says so. |
| 7 | **Enable at Startup** | Checked while `%APPDATA%\…\Startup\Tiro.lnk` exists, re-derived from the file each time the menu opens. |
| 8 | *separator* | |
| 9 | **Restart** | Hands off to a fresh process and exits (§5.8.4). |
| 10 | **About** | Opens §3.4. |
| 11 | **Exit** | Terminates immediately, no confirmation, from any state. |

There is no start/stop menu item: **left-clicking the icon** toggles dictation,
and it is the only mouse gesture that does.

### 3.4 About dialog

Modal, centred, opened from the menu or `--debug --about`; it ends dictation
first. Contents:

```
Tiro 1.0.0 (<commit hash>)

Author:     Piyush Raizada
Vibe coded with Hermes Agent (Nous Research), Qwen/Qwen3.8-Flash-Next
and deepseek-ai/DeepSeek-V4.1-Flash-1017

Log file:
<full path to app.log>

Speech model:       NVIDIA Nemotron — NVIDIA Open Model License
Punctuation model:  Horizon Labs — Apache License 2.0 (optional download)
Speech engine:      sherpa-onnx <runtime version> — Apache License 2.0
Inference runtime:  ONNX Runtime <version> — MIT License (statically linked)

Full licence texts in LICENSE.txt and NOTICE.txt beside the executable.
```

**Copy details** copies that text to the clipboard and relabels itself
"Copied". **Close** closes the dialog, never the application.

### 3.5 Configure Hotkey dialog

Modal, centred, opened from the menu or `--debug --hotkey`. It ends dictation
first and unregisters the global hotkey while open, so keystrokes reach the
dialog instead of toggling dictation.

- Shows the current combination, or `(none)` when none is registered.
- Captures key-down events only. Bare modifiers are ignored; **Esc** cancels;
  every other key is accepted whatever its glyph.
- Renders `Ctrl+Alt+Shift+Win+<key>` in that order, naming `A`-`Z`, `0`-`9`,
  `F1`-`F12`, Space, Enter, Tab, Esc, arrows, `` ` ``, `-`, `=`; anything else
  shows as `?`.
- **OK** refuses in this order: no key (*Press a combination first.*), no
  modifier (*A shortcut needs at least one modifier.*), combination owned by
  another program (*That shortcut is already in use.*, dialog stays open).
- On success the combination is registered, written to the registry, and
  `hotkey changed to <combo>` is logged.
- **Clear** unregisters and stores 0/0; the tray left-click remains.
- **Cancel** or Esc leaves the previous combination registered.
- No notification is shown for any outcome.

---

## 4. Requirement index

| ID | Area | Section |
|---|---|---|
| FR-1 | Audio capture | §5.1 |
| FR-2 | Transcription engine | §5.2 |
| FR-3 | CPU topology | §5.3 |
| FR-4 | Target selection and injection | §5.4 |
| FR-5 | State machine and toggle | §5.5 |
| FR-6 | Notifications | §5.6 |
| FR-7 | Configuration | §5.7 |
| FR-8 | Lifecycle | §5.8 |
| FR-9 | Robustness and timing | §5.9 |
| FR-10 | Logging and privacy | §5.10 |
| FR-11 | Diagnostics | §5.11 |

---

## 5. Functional requirements

### 5.1 FR-1 Audio capture

1. **Format contract.** 16 kHz, mono, 16-bit PCM, converted to float32 in
   process. Capture delivers exactly that.
2. **Windows does the conversion.** The stream is opened shared-mode with
   `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
   The audio engine's channel matrixer and sample-rate converter do the work.
   The application contains no resampler, no downmixer and no third-party audio
   library.
3. **Device choice.** With no stored device, the system default console capture
   endpoint. With a stored id, that device. The id is persisted as
   `HKCU\Software\Tiro\DeviceId` and reused across restarts.
4. **Stored device missing.** `ERROR_NOT_FOUND` produces the log line
   `saved microphone not found: falling back to the system default`, the stored
   value is cleared and the default device opens. A device that exists but will
   not open is an error (§5.9).
5. **No live default-device following.** The default is resolved when capture
   opens — at turn-on and at a microphone change — not while listening. Plugging
   in a headset mid-dictation does not move the stream; an endpoint that
   disappears surfaces through FR-1.7. This is a 1.0.0 limitation.
6. **Device list.** Active capture endpoints only, by friendly name. Disabled,
   unplugged and ghost endpoints are not listed.
7. **Device loss.** A failing `GetNextPacketSize` or a buffer flagged
   `AUDCLNT_E_DEVICE_INVALIDATED` posts device-lost: the engine stops, capture
   closes, the state returns to Off and a notification explains it. The next
   turn-on restarts the engine.
8. **Lifetime.** While On the stream stays open with no duration limit. While
   Off, capture is closed: the microphone is not held and no audio reaches the
   model.
9. **Capture thread.** One thread owns the stream: it waits on the capture event
   with a 200 ms timeout, drains every packet, and pushes samples to the engine
   queue. Packets flagged `AUDCLNT_BUFFERFLAGS_SILENT` are not pushed.

### 5.2 FR-2 Transcription engine

1. **Models are data.** The engine reads `models\<folder>\model.json` beside the
   executable; a folder counts as a model if it is a directory containing that
   manifest. The manifest must supply file names for `encoder`, `decoder`,
   `joiner` and `tokens`, relative to the folder — the shipped manifest nests
   them in a `files` object, and the reader looks for those four keys wherever
   they appear, so the exact nesting is not load-bearing. `name` is the menu
   label; without it the folder name is used. Every other field (`family`,
   `sample_rate`, `language`, `chunk_ms`, `license`, `notice`, `source`) is
   documentation the code never reads.
2. **One engine.** sherpa-onnx over ONNX Runtime, statically linked, in process,
   CPU only. No server, no Python, no helper process.
3. **Load timing.** The model loads once, on a background thread, 1.2 s after
   the message window exists — before the user dictates, so the first toggle is
   not delayed by a cold load. Loading ends the Loading state.
4. **Residency.** The recogniser survives toggles: a toggle opens or closes
   capture and gates audio. The process therefore holds the model's memory for
   its lifetime and uses no measurable CPU while Off.
5. **Decoding.** Streaming greedy search, `max_active_paths 4`, feature
   dimension 80, `model_type = "transducer"`, provider `cpu`. Inference threads
   default to 1, and sherpa-onnx applies that count to each of the encoder,
   decoder and joiner sessions, so the shipped default is three inference
   threads in total.
6. **Streaming delivery.** Audio is decoded in 100 ms slices and each slice's
   new text is typed immediately. The stream is never reset inside a session: a
   pause closes a *log segment*, not the decoder, so the next sentence keeps its
   left context. Segment boundaries (2.4 s silence, 20 s speech) are bookkeeping.
7. **No revision.** Greedy decoding appends. If a result ever diverges from what
   was typed, the divergence is logged and that utterance stops being typed
   rather than rewriting the document.
8. **Recovery.** Startup failure leaves the state Loading with a notification
   and no retry; recovery is a restart. Runtime failure, device loss and model
   change leave the engine stopped; the next turn-on restarts it, loading the
   newly selected model if one was chosen.
9. **Model change.** Stops dictation, unloads the recogniser, stores the choice,
   logs `model selected: <name> (loads at the next dictation start)`. Nothing
   reloads until the next turn-on.

### 5.3 FR-3 CPU topology

1. **Discovery.** `GetLogicalProcessorInformationEx(RelationProcessorCore)`.
2. **Hybrid detection.** Performance cores are the logical processors whose
   cores report the highest `EfficiencyClass`. On homogeneous CPUs, where every
   core reports 0, all physical cores are used and the fallback is logged at
   `DEBUG`.
3. **Pinning is process-wide.** Before the recogniser creates threads, the
   process affinity mask is set to the P-core logical set; the log records
   `core pinning: <n> P-core logicals (0x<mask>)`. Pinning is skipped, with a
   logged reason, when disabled by `PinCores=0`, when the topology cannot be
   read, on multiple processor groups, when the mask is implausible, or on a
   non-hybrid CPU.
4. **No thread-level tuning.** No per-thread affinity mask, no thread priority
   change anywhere in the application.

### 5.4 FR-4 Target selection and injection

1. **Target.** The foreground window at turn-on, unless it is the shell, the
   taskbar or the desktop, in which case the last real application window is
   used. Tiro's own windows are never targets.
2. **No target.** Turn-on with no usable target closes capture immediately (the
   microphone never stays live with nothing to type into) and notifies:
   *No window to type into. Click into the document you want to dictate to, then
   turn dictation on.*
3. **Focus hand-back.** If the target is not focused — typical for a tray
   turn-on — the app calls `SetForegroundWindow`, waits 60 ms, and logs
   `focus returned to the target window` or `focus could not be returned; click
   into the document to continue`.
4. **Window ownership.** Focus counts as inside the target when the foreground
   window is the target or one of its children.
5. **Focus watch.** A 250 ms timer ends dictation when focus moves to another
   application. Shell surfaces (taskbar, tray overflow, Tiro's own windows, start
   menu) are ignored while focus has not yet been confirmed inside the target and
   for 800 ms after turn-on, so the click that starts dictation does not stop it.
   Clicking the desktop ends dictation.
6. **Injection.** `SendInput` with `KEYEVENTF_UNICODE`: one `INPUT` per UTF-16
   code unit, down then up. No virtual-key mapping, no `WM_CHAR`, no clipboard,
   no pacing in the dictation path. Surrogate pairs are sent as their two code
   units. Nothing is substituted: newline, tab and carriage return are typed as
   themselves if the model emits them.
7. **Spacing.** Segments are appended verbatim. A single space is inserted
   before a segment only when it is the first of a session, the last character
   typed was not whitespace, and the new text does not start with whitespace. No
   trailing space is ever added.
8. **Refusals.** A segment is discarded with a log line if the target window is
   gone or is not focused when the injector is about to type. Nothing is held for
   a later window.
9. **Drain typing (§5.9.3).** After a user stop the injector may return focus to
   the target once and type the remaining tail; if focus cannot be returned the
   tail is discarded.
10. **Queue.** Up to 16 pending segments; on overflow the oldest is dropped with
    a warning. A segment that takes more than 1 s to type is logged as a warning
    with its duration.
11. **Known limitation.** `SendInput` cannot deliver keystrokes to windows at a
    higher integrity level (elevated applications, secure desktop, Task Manager)
    and the OS reports no error; text for such a window is lost silently. This is
    accepted for 1.0.0.

### 5.5 FR-5 State machine and toggle

1. **States.** `Loading`, `Off`, `On` — one global state, mirrored by the tray
   icon (§3.1) and by whether capture is open.
2. **Turn-on.** Refused with a notification when no capture device exists.
   Otherwise: restart the engine if it is stopped, open capture, record target
   and focus, set On, show the eyeball.
3. **User turn-off.** Close capture first, then drain (§5.9.3), then Off.
4. **Forced turn-off.** Focus loss, device loss, engine failure, opening a
   dialog or changing the model stop immediately: injector queue cleared,
   pending audio dropped, capture closed, Off. In-flight words are discarded by
   design.
5. **Loading.** Toggling while Loading is ignored and logged; Exit works from
   every state.
6. **Click guard.** A turn-on within 500 ms of an automatic focus-loss stop is
   ignored, so the click that moved focus away cannot immediately restart
   dictation.
7. **Only two toggle surfaces.** Tray left-click and the global hotkey.

### 5.6 FR-6 Notifications

1. **One at a time.** A message replaces the current message and the eyeball;
   a turn-on replaces a message with the eyeball.
2. **Behaviour.** 5 s lifetime, topmost, no activation, no taskbar entry,
   primary monitor, right-aligned 16 px above the work-area bottom, sized to its
   text, never interactive.
3. **The complete message set:**

| Situation | Text |
|---|---|
| Turn-on with no capture device | `No microphones available to transcribe.` |
| Device busy | `Could not open the microphone - it may be in use by another application.` |
| Audio service stopped | `The Windows audio service is not running.` |
| Endpoint creation failed | `The microphone could not be opened.` |
| Format refused | `The microphone does not support 16 kHz mono audio.` |
| Other open failure | `The microphone could not be opened.` |
| No target window | `No window to type into. Click into the document you want to dictate to, then turn dictation on.` |
| Focus left the target | `Dictation stopped: "<window title>" lost focus.` |
| Capture device lost | `The microphone used for transcription was lost. Dictation stopped.` |
| Engine failed to load | `Transcription engine failed to load. See the log file.` |
| Polish enabled, model absent | `Punctuation model not found. It is optional and not part of the download — see README.txt to add it, then restart Tiro.` |
| Punctuation enabled | `Punctuation will be enabled after Tiro is restarted.` |
| Punctuation disabled | `Punctuation will be disabled after Tiro is restarted.` |
| Startup folder unavailable | `Could not find your Startup folder.` |
| Startup shortcut failed | `Could not create the Startup shortcut.` |
| Restart hand-off failed | `Could not restart.` |
| Diagnostic test | `This is a test notification. It disappears after five seconds.` |

4. **No notification** for: a successful turn-on (the eyeball is the feedback), a
   user stop, hotkey change or clear, successful microphone or model change,
   successful Startup shortcut change, engine-ready, About or Exit. All are
   logged.

### 5.7 FR-7 Configuration

All settings are values under `HKCU\Software\Tiro`. The application writes only
six of them.

| Value | Type | Default | Written by | Meaning |
|---|---|---|---|---|
| `DeviceId` | REG_SZ | `""` | microphone change, fallback | Endpoint id; empty = system default |
| `ModelFolder` | REG_SZ | `""` | model change | Model subfolder; empty = first usable model |
| `HotkeyMods` | REG_DWORD | `3` (Ctrl+Alt) | hotkey dialog | Modifier mask |
| `HotkeyVk` | REG_DWORD | `32` (Space) | hotkey dialog | Virtual key; 0 = no global hotkey |
| `ShowEye` | REG_DWORD | `1` | tray toggle | 0 disables the eyeball entirely |
| `Polish` | REG_DWORD | `0` | tray toggle | 1 enables the rewrite at the next start |

Read-only tuning values (no UI exposes them): `LogText` 0, `RecordWav` 0,
`PinCores` 1, `Threads` 0 (→1), `PolishThreads` 1, `SilenceRmsPct` 1 (→0.01
RMS), `PauseMs` 2400 (min 200), `MaxUtteranceMs` 20000, `Rule1Ms`/`Rule2Ms`
2500, `Rule3Ms` 15000, `SliceMs` 100, `ChunkMs` 100 (self-test only), `EyeGain`
1600, `EyeAttack` 750, `EyeRelease` 280.

1. **Default hotkey** is `Ctrl+Alt+Space`, registered with `MOD_NOREPEAT`. A
   registration failure at startup is logged at `WARN`; the app continues
   without a hotkey and the tray left-click still works.
2. **No hotkey.** `HotkeyVk = 0` registers nothing.
3. **Re-registration.** At startup, after the dialog closes, and after an
   Explorer restart.
4. **Notification theme** follows
   `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`,
   defaulting to light when absent.

### 5.8 FR-8 Lifecycle

1. **Single instance.** Named mutex `Local\Tiro.SingleInstance`. A second
   launch posts a message to the first window and exits 0 without a window,
   balloon or notification; the first logs
   `second instance was refused: already running`.
2. **Debug bypass.** `--debug` skips the check and logs
   `debug mode: running alongside the live instance`, so diagnostics can run
   beside a live tray copy. Two icons and two hotkey registrations are possible
   in that mode and that is intended.
3. **Startup registration.** **Enable at Startup** creates or deletes
   `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Tiro.lnk`, a shell
   link to the executable with its working directory set to the executable's
   folder, so `models\` resolves. No Run key is used; the menu check reflects the
   file's existence.
4. **Restart.** Duplicates its own process handle, launches
   `<exe> --wait-handle <handle>` detached and exits. The new process waits up to
   15 s for the old one before taking the mutex.
5. **Shutdown.** Injector stopped (5 s grace), engine unloaded, capture closed,
   hotkey unregistered, tray icon deleted, `shutdown` logged.

### 5.9 FR-9 Robustness and timing

1. **No microphone.** Turn-on refused with a notification; state stays Off;
   capture never opens.
2. **Engine missing or corrupt at startup.** State stays Loading with a
   notification and an error; restart the application after fixing the model.
3. **User stop drains.** A user stop closes the microphone first, then lets the
   decoder finish what was already spoken:
   - the engine decodes the queued audio for up to **2 s**; past the deadline the
     remainder is dropped with `drain: deadline passed, the rest of the backlog
     is dropped`;
   - the app then waits up to **3 s** (20 ms poll) for the injector to finish;
   - polish, when enabled, waits up to a further **3 s** for delivery to settle;
   - the log then records `drain: tail released after user stop`.

   The intent is that the last words spoken are typed, not deleted. Forced stops
   skip all of this and discard the tail.
4. **Forced stop.** Focus loss, device loss, engine failure, dialog or model
   change: queue cleared, pending audio dropped, capture closed, Off.
5. **Backpressure.** The engine queue holds 20 s. When inference falls behind,
   the **oldest** one second is discarded per push and the shed total is warned
   about on the first and every tenth event; the state stays On, so dictation
   degrades rather than dropping out.
6. **Injector backpressure.** 16 segments, oldest dropped (§5.4.10).
7. **Recovery summary.** Lost device, changed model and runtime engine failure
   leave the engine stopped; the next turn-on restarts it. Startup failure is
   never retried automatically.

### 5.10 FR-10 Logging and privacy

1. **File.** `%LOCALAPPDATA%\Tiro\logs\app.log`, UTF-8, one line per event,
   opened-appended-closed per write so it can be read while the app runs.
2. **Format.** `YYYY-MM-DD HH:MM:SS [LEVEL] message` — local time, no
   milliseconds, no thread id.
3. **Levels.** `DEBUG`, `INFO`, `WARN`, `ERROR`, all always written; there is no
   filtering and no verbosity setting. `DEBUG` covers loaded icon handles and a
   periodic `audio: peak=<n>% frames=<n>` meter line.
4. **Rotation.** At startup only: an `app.log` of 2 MB or more is moved to
   `app.log.old`, one generation kept. A running session never rotates.
5. **Speech is not logged by default.** With `LogText=0` the log records
   `utterance (…)` lengths and `inject: <n> chars`, never the words. `LogText=1`
   records the words — a support switch, read once per process, never enabled by
   the UI.
6. **Audio is not recorded by default.** With `RecordWav=1` each engine session
   writes `capture-YYYYMMDD-HHMMSS.wav` (16 kHz mono 16-bit PCM, exactly what the
   recogniser received) into the logs folder.
7. **Self-test is the exception.** It logs its transcript regardless of
   `LogText` (§5.11.1).
8. **Never logged:** clipboard contents, target window contents, or audio beyond
   the meter line.
9. **No Event Log.** A portable executable cannot register an Event Log source
   without writing to HKLM, so the file is the log of record. No hooks of any
   kind are installed, and the process makes no network calls.

### 5.11 FR-11 Diagnostics

Diagnostic switches are inert without `--debug`: each logs
`diagnostic switch ignored (needs --debug): <switch>` and does nothing. `--debug`
changes exactly two things — it bypasses single instance, and it enables these
switches. It grants no privilege, starts no dictation, and touches neither
`LogText` nor `RecordWav`.

| Switch | Behaviour |
|---|---|
| `--debug` | Enables the rest; bypasses single instance; logs that it did |
| `--about` | Ends dictation, opens About, quits |
| `--hotkey` | Ends dictation, opens the hotkey dialog, quits |
| `--notify` | Shows the test notification, quits after 6 s |
| `--startup-toggle` | Creates or removes the Startup shortcut, quits |
| `--restart` | Same hand-off as the menu item |
| `--selftest <wav>` | Replays a WAV through the real recogniser (§5.11.1) |
| `--live <wav> [--target <title>]` | Feeds a WAV into the live pipeline in the On state, optionally pinning a window by title, then keeps running |
| `--rewrite-demo` | One-shot rewrite test against an owned RICHEDIT50W window; logs PASS/FAIL and quits |
| `--polish-file <in> <out>` | Runs the polish model over a UTF-8 file; exit 2 if the model will not load, 3 on I/O failure |
| `--polish-tokens <in> <out>` | Dumps token ids for verifying the tokenizer |

1. **Self-test.** `--selftest` loads the selected model, feeds the WAV in 100 ms
   chunks through the same stream and segmenter as live dictation, and logs
   `selftest: <n> s of audio`, one `selftest segment [pause|long]: <delta>` per
   segment, `selftest final: <full transcript>` and `selftest: done`. It never
   opens the microphone, writes nothing to stdout or stderr, and **always exits
   0** — the transcript is the result, not the exit code. A wrong-format WAV logs
   an error and still exits 0.
2. **Log-only output.** A GUI process has no console; every diagnostic reports
   through `app.log`.

---

## 6. Technical architecture

### 6.1 Process shape

One process, one message window, one tray icon, and up to six threads: UI,
capture, engine, injector, polish, polish-init. No service, no watchdog, no
second process except the deliberate restart hand-off.

### 6.2 Capture pipeline

`IAudioClient` (shared, event-driven, auto-converted) → capture thread → sample
queue → engine thread. The capture thread waits on a Windows event with a 200 ms
timeout, drains every packet, skips silent ones, and pushes 16-bit mono samples.

### 6.3 Queue and backpressure

The engine queue is a deque of float samples capped at 20 s (320,000 samples).
A push that would exceed the cap discards the oldest second; the shed counter is
reported on the first and every tenth event
(`audio: inference behind, shed <n> s of oldest audio in total`). Dropping the
oldest keeps latency bounded and keeps the speech the user is waiting for.

### 6.4 Inference

The engine thread wakes on a 50 ms condition-variable wait, takes the whole
queue, clears it, and decodes in 100 ms slices while the stream reports ready.
After each slice the new text goes straight to the injector. On a segment
boundary (2.4 s silence or 20 s speech) the current delta is delivered and the
boundary bookkeeping resets — never the stream.

The model window is 560 ms. Endpoint rules are configured in the recogniser
(2500/2500/15000 ms) but the application never consults them, because the stream
is never reset.

### 6.5 Injection

One injector thread and one work queue. Delivery is `SendInput` with
`KEYEVENTF_UNICODE` per code unit under a "delivering" flag that the polish pass
waits on. Focus is checked before typing; a segment whose target is not
foreground is dropped, except during a user-stop drain where focus is returned
once.

### 6.6 Notifications

One layered pop-up class for messages and one for the eyeball, both
`WS_EX_NOACTIVATE | WS_EX_TOPMOST` tool windows. Messages size themselves to
their text and dismiss on a 5 s timer. The eyeball animates on a 33 ms timer from
the RMS of captured audio, with slow reference tracking so it opens for quiet
speech and stays shut in a noisy room.

### 6.7 Optional punctuation rewrite

Off by default; the switch is restart-scoped so it can never fire mid-session.
With `Polish=1`, a second ONNX model at `models\punctuation\`
(`tokenizer.json`, `config.json` containing `id2label`, and
`model_quantized.onnx`) is loaded on a background thread at startup. At a
**user** stop only, the session text is rewritten in process — punctuation
restored at word ends, sentence starts capitalised, standalone "i" → "I",
whitespace collapsed, 400-token windows with 50-token overlap — and the target is
updated in one act: for classic `EDIT`/`RichEdit` controls, `EM_EXSETSEL` plus
`EM_REPLACESEL` are queued back-to-back so the control replaces the dictated span
inside its own thread without ever painting a selection. Targets that do not
expose such a control (Chromium-family, WinUI) are left exactly as typed; Tiro
never borrows the clipboard and never sends backspace or selection keystrokes.

Failure is always safe: if the model is not ready, the output is empty, the
target is no longer foreground, the target does not support the one-shot
replace, or the rewrite is interrupted, the raw text
stands and the reason is logged. Forced stops never polish.

### 6.8 Packaging

`Tiro.exe` is a single static binary (about 19 MB): `/MT` C runtime, sherpa-onnx,
ONNX Runtime and the audio stack all linked in. No DLLs, no redistributable
runtime, no installer. A release build is checked with `dumpbin /dependents` and
must import only Windows system libraries.

The application is unsigned by design; SmartScreen warns on a downloaded copy
and `README.txt` explains the "More info → Run anyway" path.

Models live in `models\` beside the executable and are read in place — nothing is
extracted, copied or cached. A model folder is added by copying it, removed by
deleting it.

**Release artifact** (`make_release.bat <version>`, run only from a clean,
committed tree):

```
Tiro\
  Tiro.exe
  README.txt
  LICENSE.txt
  NOTICE.txt
  SECURITY.md
  models\nemotron-0.6b-en\   (manifest, four model files, LICENSE.txt, NOTICE.txt)
```

The script refuses a dirty tree, creates the `v<version>` tag if absent, stages
that folder, writes `build\Tiro-<version>-win64.zip`, re-opens the archive to
verify the entry list and that the packaged exe hashes equal to the built one,
and writes a SHA-256 beside the zip. Upload is manual. Development builds produce
no zip.

**Licence obligations.** Nemotron is distributed under the NVIDIA Open Model
License: recipients must receive the agreement and a notice containing the
sentence "Licensed by NVIDIA Corporation under the NVIDIA Open Model License",
and NVIDIA marks may be used only to describe the model's origin. sherpa-onnx is
Apache-2.0; ONNX Runtime is MIT; the optional punctuation model is Apache-2.0.
The zip carries `LICENSE.txt`, `NOTICE.txt` and per-model licence files for
exactly this reason.

---

## 7. BDD scenarios

Each feature names the requirements it covers. The fixture
`test/fixtures/punct_selftest.wav` is the reference audio for the self-test
scenarios.

### Feature: Startup and load
*FR-2.3, §3.1, FR-9.2*

```gherkin
Scenario: Engine ready after startup
  Given a valid model in models\nemotron-0.6b-en
  When the application starts
  Then the tray icon shows the pale loading arc with tooltip "Tiro - starting"
  And the log records "engine ready in <n> ms (threads=1)"
  And the tray icon becomes the red glow with tooltip "Tiro - off"

Scenario: No usable model at startup
  Given models\ contains no folder with a complete manifest
  When the application starts
  Then the notification reads "Transcription engine failed to load. See the log file."
  And the tray icon stays on the pale loading arc
  And the microphone and model submenus are greyed out
  And Exit still works
```

### Feature: Toggle dictation
*FR-5*

```gherkin
Scenario: Turn on with a focused document
  Given a text editor has focus
  When the user presses the hotkey
  Then capture opens at 16 kHz mono and the log records "dictation on: capture open at 16 kHz mono"
  And the tray icon turns green with tooltip "Tiro - listening"
  And the eyeball pop-up appears

Scenario: Turn on from the tray with focus elsewhere
  Given the taskbar has focus
  When the user left-clicks the tray icon
  Then the last real application window becomes the target
  And focus is returned to it, or the failure to do so is logged
  And dictation is On

Scenario: Toggle during loading is ignored
  Given the icon still shows the pale loading arc
  When the user presses the hotkey
  Then the log records "toggle ignored while loading"
  And the state does not change

Scenario: The click that ended dictation does not restart it
  Given dictation stopped because the target lost focus
  When that same click reaches the tray icon within 500 ms
  Then the log records "toggle ignored: it is the click that just ended dictation"
```

### Feature: Stop and drain
*FR-9.3, FR-9.4*

```gherkin
Scenario: A user stop types the last words
  Given dictation is On and the user has just finished a sentence
  When the user presses the hotkey to stop
  Then capture closes immediately
  And the decoder finishes the already-captured audio, for at most 2 s
  And the log records "drain: tail released after user stop"

Scenario: The drain hits its deadline
  Given decoding is too slow to finish within 2 s
  When the user stops
  Then the log records "drain: deadline passed, the rest of the backlog is dropped"
  And the state is Off

Scenario: A forced stop discards the tail
  Given dictation is On
  When the target window loses focus
  Then the injector queue is cleared and pending audio dropped
  And capture closes and the state is Off
  And the notification names the window that took focus
```

### Feature: Focus guard
*FR-4.1, FR-4.4, FR-4.5, FR-4.8*

```gherkin
Scenario: Typing follows the user away
  Given dictation is On with a text editor as the target
  When the user clicks a browser window
  Then dictation stops and the browser receives no text
  And the notification reads "Dictation stopped: "<title>" lost focus."

Scenario: Clicking the tray to start does not stop dictation
  Given dictation is Off and the user clicks the tray icon
  When focus settles on the target
  Then dictation stays On and the next words reach the target

Scenario: The desktop ends dictation
  Given dictation is On
  When the user clicks the desktop
  Then dictation stops with the focus-lost notification

Scenario: No target at all
  Given no application window has held focus since login
  When the user turns dictation on
  Then capture closes again immediately
  And the notification reads "No window to type into. ..."
```

### Feature: Text injection
*FR-4.6, FR-4.7*

```gherkin
Scenario: Words appear while speaking
  Given dictation is On and the caret is in a text field
  When the user says "the quick brown fox"
  Then the words appear progressively, not all at the pause
  And they match the model output verbatim

Scenario: Non-ASCII text
  Given the model emits a non-Latin word
  When it is injected
  Then it arrives intact as Unicode, with no keyboard-layout dependency

Scenario: Segment spacing
  Given a segment ending in a letter has been typed
  When the next segment starts with a letter
  Then exactly one space separates them
  And no trailing space is added at the end of the session
```

### Feature: Microphone selection
*FR-1.3, FR-1.4, FR-1.6*

```gherkin
Scenario: Choosing a microphone while off
  Given dictation is Off
  When the user picks a device
  Then the id is stored and the log records "microphone selected: <name>"
  And the next turn-on opens that device

Scenario: Switching while listening
  Given dictation is On
  When the user picks another device
  Then capture reopens on it without stopping dictation
  And if it will not open, the previous device is restored and the reason notified

Scenario: The stored device disappeared
  Given the stored endpoint no longer exists
  When the user turns dictation on
  Then the log records "saved microphone not found: falling back to the system default"
  And capture opens on the default device
```

### Feature: Device loss
*FR-1.7, FR-2.8*

```gherkin
Scenario: Microphone unplugged while listening
  Given dictation is On
  When the capture endpoint is invalidated
  Then the engine stops and capture closes
  And the notification reads "The microphone used for transcription was lost. Dictation stopped."
  And the icon returns to the red glow

Scenario: Dictation works again afterwards
  Given the engine stopped after a device loss
  When the user turns dictation on again
  Then the engine restarts and dictation transcribes normally
```

### Feature: Model selection
*FR-2.1, FR-2.8, FR-2.9*

```gherkin
Scenario: Switching models
  Given two valid model folders
  When the user selects the other one
  Then dictation stops if it was on and the recogniser is unloaded
  And the log records "model selected: <name> (loads at the next dictation start)"

Scenario: The new model loads at the next turn-on
  Given a model was just selected
  When the user turns dictation on
  Then the engine starts with that model
  And a fresh "engine ready in <n> ms" appears in the log

Scenario: Manifest missing required file names
  Given a model.json without "tokens"
  When the engine loads it
  Then the log records "model manifest is missing required file names"
  And the failure surfaces as "Transcription engine failed to load."
```

### Feature: Hotkey dialog
*§3.5*

```gherkin
Scenario: Accepting a valid combination
  Given the dialog is open
  When the user presses Ctrl+Shift+F9 and clicks OK
  Then the combination is registered and stored
  And the log records "hotkey changed to Ctrl+Shift+F9"

Scenario: Refusing a bare key
  When the user presses F9 alone and clicks OK
  Then the dialog stays open with "A shortcut needs at least one modifier."

Scenario: Refusing a combination another program owns
  Given another application owns Ctrl+Alt+T
  When the user captures Ctrl+Alt+T and clicks OK
  Then the dialog stays open with "That shortcut is already in use."
  And the previous hotkey stays active

Scenario: Clearing and cancelling
  When the user clicks Clear
  Then no hotkey is registered and the tray left-click still toggles
  When the user instead presses Esc
  Then nothing is written and the previous hotkey stays registered
```

### Feature: Tray icon states
*§3.1*

```gherkin
Scenario: Three visually distinct states
  Then Loading shows pale arcs around the collar
  And Off shows a red band on the lower rim with a dark lens
  And On shows a green band on the lower rim with a lit lens
  And each remains legible at 16x16

Scenario: Explorer restarts
  Given the shell restarted
  When the TaskbarCreated message arrives
  Then the icon is re-added in its current state
  And the hotkey is registered again
```

### Feature: Notifications
*FR-6*

```gherkin
Scenario: A message replaces the eyeball
  Given the eyeball is visible while dictation is On
  When an error notification is shown
  Then the eyeball disappears and the opaque panel takes its place

Scenario: Messages expire on their own
  When a notification is shown
  Then it disappears after five seconds without interaction
  And its text is in the log at INFO

Scenario: No notification for ordinary success
  When dictation turns on and stays healthy
  Then no text panel appears
  And only the eyeball confirms that the microphone is heard
```

### Feature: Self-test and diagnostics
*FR-11*

```gherkin
Scenario: Replaying the fixture without a microphone
  Given no capture device is available
  When the application runs with --debug --selftest and the fixture
  Then nothing is printed to the console and the process exits 0
  And the log records "selftest: <n> s of audio"
  And the log records "selftest final: Hello there. Who are you? I like tea but not coffee"

Scenario: A diagnostic switch without --debug
  When the application starts with --about and without --debug
  Then the log records "diagnostic switch ignored (needs --debug): --about"
  And the application starts normally
```

### Feature: Single instance and restart
*FR-8.1, FR-8.2, FR-8.4*

```gherkin
Scenario: Second launch is refused quietly
  Given Tiro is already running
  When the user launches it again
  Then the second process exits 0 with no window or notification
  And the running instance logs "second instance was refused: already running"

Scenario: Debug mode runs alongside
  When the user launches with --debug while a tray copy runs
  Then both run and the log records "debug mode: running alongside the live instance"

Scenario: Restart hand-off
  When the user chooses Restart
  Then a new process waits for the old one to exit and then starts
  And the tray icon reappears with the same settings
```

### Feature: Startup registration
*FR-8.3*

```gherkin
Scenario: Enabling at startup
  When the user ticks "Enable at Startup"
  Then %APPDATA%\...\Startup\Tiro.lnk is created pointing at the executable
  And its working directory is the executable's folder
  And the menu item shows a check while that file exists

Scenario: Disabling at startup
  When the user unticks it
  Then the shortcut is deleted and the log records "startup: shortcut removed - <path>"
```

### Feature: Optional punctuation
*FR-6, FR-7, §6.7*

```gherkin
Scenario: Off by default
  Given a fresh install
  When the user dictates a sentence
  Then the model's own punctuation is typed and nothing is rewritten

Scenario: Enabling is restart-scoped
  Given "Disable Punctuation" is ticked
  When the user unticks it
  Then the notification reads "Punctuation will be enabled after Tiro is restarted."
  And nothing changes in this session

Scenario: Model absent
  Given Polish is enabled but models\punctuation is missing
  When the application starts
  Then the notification explains it is an optional download
  And dictation still works with raw model text

Scenario: The rewrite lands in one step
  Given punctuation is enabled and ready
  And the target exposes a classic edit or RichEdit control
  When the user stops dictating with the hotkey
  Then the dictated span is replaced in one act
  And no selection highlight is ever painted
  And the log records "polish: <a> chars -> <b> chars via replace-sel in <n> ms"

Scenario: An unsupported target keeps its text
  Given punctuation is enabled and ready
  And the target is a Chromium or WinUI control
  When the user stops dictating with the hotkey
  Then the dictated text remains exactly as injected
  And the log records "polish: target does not support one-shot replace"
```

### Feature: Capability boundaries
*§2.2, FR-1.5, §6.8*

```gherkin
Scenario: No network
  Given the application is running
  When it transcribes and types
  Then the process makes no outbound connection

Scenario: The default device changes while listening
  Given dictation is On using the system default microphone
  When a headset is plugged in and Windows changes the default
  Then the running capture is unaffected for this session
  And the change applies from the next turn-on

Scenario: Only system libraries are imported
  Given a release build
  When dumpbin /dependents is run
  Then every imported module is a Windows system library
```

### Feature: Logging and privacy
*FR-10*

```gherkin
Scenario: Words stay out of the log by default
  Given LogText is 0
  When the user dictates "quarterly numbers are confidential"
  Then the log records "inject: <n> chars"
  And those words appear nowhere in app.log

Scenario: Rotation at startup
  Given app.log is 2 MB or larger
  When the application starts
  Then app.log becomes app.log.old and a new app.log begins
  And a running session never rotates its own log
```

### Feature: Slow hardware
*FR-9.5*

```gherkin
Scenario: Inference falls behind
  Given the CPU cannot decode in real time
  When the queue reaches 20 s
  Then the oldest second is discarded on each further push
  And the log records "audio: inference behind, shed <n> s of oldest audio in total"
  And dictation stays On instead of dropping out
```

---

## 8. Verification map

| Requirement | Evidence |
|---|---|
| FR-1 capture | 16 kHz mono logged at turn-on; self-test uses the same stream |
| FR-2 engine | `engine ready in <n> ms`; self-test transcript |
| FR-3 topology | `core map:` and `core pinning:` lines |
| FR-4 injection | live dictation; `--rewrite-demo` for the one-shot rewrite |
| FR-5 state machine | tray icon states and state-change log lines |
| FR-6 notifications | `--debug --notify`; the message table above |
| FR-7 configuration | registry inspection; hotkey dialog behaviour |
| FR-8 lifecycle | second launch; `--debug` alongside; Restart |
| FR-9 robustness | self-test; drain log lines; shed warnings |
| FR-10 logging | `app.log` with `LogText` 0 and 1 |
| FR-11 diagnostics | the switch table; `--selftest` transcript |

**Release gate:** clean build; the About/stamp hash equals
`git rev-parse --short HEAD`; `--debug --selftest` on the fixture produces the
reference transcript from that same hash; the packaged zip contains the
executable, four documents and the model folder with its licence files.

---

## 9. Glossary

- **Session** — one On period, from turn-on to stop.
- **Segment** — speech closed by 2.4 s of silence or 20 s of continuous audio;
  log bookkeeping only, never a decoder reset.
- **Drain** — the ≤2 s after a user stop during which the decoder finishes audio
  that was already captured.
- **Polish** — the optional punctuation rewrite in §6.7.
- **Target** — the window that receives injected text.
- **Shell surface** — taskbar, tray overflow, start menu, Tiro's own windows; not
  a target, and ignored when judging whether focus left.
