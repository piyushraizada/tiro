# Provenance

Every file in this repository and in the release archive, with where it came from and under what
right it is distributed. Audited 2026-11-27 against commit `0b35c76`.

The rule that keeps this file honest: **a file with no stated origin does not ship.** When you add
a file, give it a row here in the same commit - a release with a new asset-extension class and no
row is a release that was never audited.

## 1. Authored for this project

Written by the project's author. No third-party rights attach, and nothing is owed beyond the
licence the author chose for the whole work (section 2).

| Path | What it is |
|---|---|
| `src/main.cpp`, `src/punctuation.cpp`, `src/punctuation.h`, `src/resource.h` | the application |
| `src/Tiro.rc` | resources: version block, embedded icons |
| `res/tiro_on.ico`, `res/tiro_off.ico`, `res/tiro_loading.ico` | tray and window icons, drawn for this app (see the artwork clause in `dist/NOTICE.txt`) |
| `Tiro.sln`, `Tiro.vcxproj` | build files |
| `build_release.bat`, `make_release.bat` | build and release gating |
| `.gitignore`, `.gitattributes` | repository configuration |
| `README.md`, `BUILD.md`, `SECURITY.md`, `AGENTS.md`, `CLAUDE.md` | documentation |
| `requirements_and_bdd.md` | the specification |
| `dist/README.txt`, `dist/NOTICE.txt` | end-user manual and attribution notices |
| `dist/LICENSE.txt` | a compilation, assembled here, of the third-party texts in section 3 |
| `dist/models.sha256` | the pinned SHA-256 of every file in the shipped speech model; `make_release.bat` refuses to package a model that does not match it, and the file ships in the zip as `models/nemotron-0.6b-en/SHA256SUMS.txt` |
| `tests/uia-target.html` | the browser page the automated rewrite test drives: one `contenteditable` and one `textarea`, titled `TIRO-UIA-TARGET` so `--uia-rewrite` can find it |
| `tests/run-rewrite-tests.bat` | the automated rewrite regression: classic-edit test, then the Chromium test above; exits non-zero on failure |
| `tests/caps/*` | capture harness for the venue/overlay checks (frames, montage, runner) |
| `test/fixtures/punct_selftest.wav` | recorded fixture for `--selftest` |

## 2. The work itself

| Path | Licence | Origin |
|---|---|---|
| `LICENSE` | MIT, `Copyright (c) 2026 Piyush Raizada` | the author's own licence, chosen for this work |

## 3. Third-party material inside the repository

| Path | Component | Version pinned | Licence | Licence text shipped with it |
|---|---|---|---|---|
| `third_party/include/onnxruntime_c_api.h` | ONNX Runtime C API header | `ORT_API_VERSION 20` (the API level the header exposes; the runtime linked in is 1.28.2, which is the version the binary itself reports in About) | MIT, © Microsoft Corporation | `third_party/licenses/onnxruntime-LICENSE.txt` |
| `third_party/include/sherpa-onnx-c-api.h` | sherpa-onnx C API header | v2.0.3 | Apache-2.0, © 2023 Xiaomi Corporation | `third_party/licenses/sherpa-onnx-LICENSE.txt` |
| `third_party/sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.xz` | the 14 static libraries the app links, repacked from upstream's `.tar.bz2` with `xz -9` (identical bytes, verified by hashing every member) | v2.0.3 | Apache-2.0 (sherpa-onnx) and MIT (ONNX Runtime) | both texts, above |

Upstream: <https://github.com/k2-fsa/sherpa-onnx/releases/tag/v2.0.3>
(`sherpa-onnx-v2.0.3-win-x64-static-MT-Release-no-tts-lib.tar.bz2`)
Archive SHA-256: `027833f945cd9d3c1f417714bfad16c3624a43fa039ada241a181a10678d1353`
Header and version notes for both: `third_party/licenses/README.txt`

## 4. Test fixture

| Path | Origin |
|---|---|
| `test/fixtures/punct_selftest.wav` | Synthesised speech generated for this project. The sentence (`Hello there. Who are you? I like tea but not coffee`) was written for the test. Not a human recording, not taken from any corpus or third-party audio. 6.14 s, 16 kHz mono 16-bit PCM, 196,686 bytes. Confirmed by the author, 2026-11-27. |

It is forced into Git with `git add -f` because `*.wav` is ignored.

## 5. Shipped in the release archive but not in Git

| Path in the zip | Component | Licence | Obligation and how it is met |
|---|---|---|---|
| `Tiro/models/nemotron-0.6b-en/` | NVIDIA Nemotron speech streaming model, 0.6B, int8 | NVIDIA Open Model License | full agreement ships as `LICENSE.txt` inside the model folder, and `NOTICE.txt` inside it carries the exact required sentence: *Licensed by NVIDIA Corporation under the NVIDIA Open Model License*. `SHA256SUMS.txt` inside the same folder pins every file's hash, so the weights that ship can be re-verified by anyone holding the zip. |
| `Tiro/Tiro.exe` | built from this tree; statically links the section 3 components | Apache-2.0 and MIT | both licence texts ship as `LICENSE.txt` at the top of the archive; `dumpbin /dependents` gate proves the build carries no third-party DLL, only their code |
| `Tiro/LICENSE.txt`, `Tiro/NOTICE.txt`, `Tiro/README.txt`, `Tiro/SECURITY.md` | documents | assembled here | sections 1 and 3 above |

Model source: <https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b>

## 6. Not bundled; offered as an optional user download

| Component | Licence | Note |
|---|---|---|
| `models/punctuation` (optional punctuation pass, off by default) | Apache-2.0 | <https://huggingface.co/Horizon-Labs/punctuation-restoration-base>. The user downloads it; Tiro does not bundle or fetch it. If it is ever bundled into a release, that project's licence text must ship inside its model folder, exactly as the Nemotron folder does. |

| `tests/uia-target.html`, `tests/run-rewrite-tests.bat` | Original to this project. A marker-titled page and a three-test runner for the one-shot rewrite. | Original work; no third-party content. |
| `tests/make-voice.ps1`, `tests/run-app-suite.ps1`, `test/fixtures/voice_suite.wav` | Original to this project. The synthetic-voice fixture (generated with the Windows speech synthesiser, so it carries no recorded voice or third-party audio) and the cross-application runner that feeds it. | Fixture is machine-generated speech from the local Windows voice; the runner is original work. |

## 7. Re-running this audit

```bash
git ls-files | sed 's/.*\.//' | sort | uniq -c | sort -rn     # what kinds of files ship
python -c "import zipfile;[print(n) for n in zipfile.ZipFile('build/Tiro-1.1.1-win64.zip').namelist()]"
git grep -niE '<character>|<film>|<studio>|<brand>' -- . ':!third_party/*.tar.xz'   # expect nothing
strings bin/Release/Tiro.exe | grep -iE 'https?://|copyright' | sort -u             # foreign code inside
```

The zip's entry list is the archive's true inventory; the tree can differ from it. Every file must
land in section 1, 2 or 3. Anything else is either given a row with evidence, or deleted.
