# Changelog

## 1.1.1

* **The polished sentence now arrives in browsers.** In Chromium-based
  applications the correction could be refused in the instant it was ready;
  Tiro now waits for the application to catch up before making the change, so
  the sentence you see is the punctuated one.
* **Fast starts and stops stay in step.** Toggling dictation quickly could leave
  the recogniser and the text on screen disagreeing; that hand-off is now
  ordered, so the seam between two turns always reads the same way.
* **A bad file or a stuck device can no longer take the app down.** Audio files
  with impossible lengths are refused cleanly, and if a recording device stops
  answering, that session is abandoned instead of touching memory it may still
  be using.
* **Hardened against tampered files.** The text engine is loaded only from the
  Windows system folder, model files are checked against pinned hashes when a
  release is built, and the binary is compiled with control-flow and address
  randomisation protection enabled.

## 1.1.0

* **Every sentence you dictate ends complete.** Stop dictating and the
  sentence closes itself: the period arrives, the next sentence starts with
  its capital, and the seam between them reads like typing.
* **Your first words are never lost again.** Slow-to-respond applications
  used to swallow whatever you said the instant the hotkey went down; Tiro
  now hears you the moment you press it.
* **Punctuation is never lost.** Your words keep the punctuation as you
  speak it, and where an application accepts the seamless polish, the
  finished form replaces it in one clean instant — nothing selected,
  nothing removed, nothing retyped. Where an application refuses, what you
  spoke stands exactly as written, period included.
* **Works in more places.** Full support now spans Word, Chrome and other
  Chromium apps, Notepad++, VS Code and Antigravity — including their chat
  panels — and classic text edit controls.

## 1.0.0

* First release. Tiro is a 100% local, portable dictation tool: one hotkey,
  system-wide typing, the NVIDIA Nemotron speech model, and the speech
  model's own native punctuation — no cloud, no installation.
