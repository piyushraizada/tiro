# Tiro: Your Private Voice-to-Text Companion

**Tiro** is a high-speed, 100% private transcription tool designed for people who need to turn their voice into text instantly—without ever sending their data to the internet.

## 🌟 Why Tiro?
Most voice-to-text tools send your voice recordings to a server (the cloud). This means your private conversations, business ideas, and personal notes are being stored by someone else.

**Tiro is different:**
*   **100% Private:** Everything happens on your computer. Your voice never leaves your machine.
*   **Runs on CPU Only:** No graphics card (GPU) needed. If you have a normal Windows PC or laptop, Tiro runs on it — nothing to buy, nothing to upgrade.
*   **No Installation:** No "Admin Rights" or complicated setups. It is a portable application.
*   **Fast & Fluid:** Designed to keep up with your natural speaking pace.
*   **Punctuation, Handled Smartly:** The Nemotron model punctuates your speech as it hears it — periods, question marks, commas — out of the box, no settings, no cloud. With the optional punctuation model, supported applications get a stricter polish that lands in one seamless instant the moment you stop talking.
*   **Zero Footprint:** It stays out of your way until you need it.

---

## 🚀 Get Tiro (In 3 Simple Steps)
You don't need to be a "tech person" to use Tiro. It is designed to work out of the box.

1.  **Download & Unzip:** Grab the latest release zip from the [Releases page](https://github.com/piyushraizada/tiro/releases) - it is named `Tiro-<version>-win64.zip`, with a matching `.sha256` beside it - and unzip it to any location on your computer (like your Desktop or Documents).
2.  **Run the App:** Double-click `Tiro.exe`. Once it's running, it sits quietly in your system tray (the area near your clock) waiting for you. Windows may warn once that the app is unsigned — choose "More info", then "Run anyway".
3.  **Read the Manual:** Open **`README.txt`** inside the Tiro folder. It is the complete guide for end users: first dictation, microphone and hotkey setup, the tray menu, punctuation options (including the optional AI upgrade), troubleshooting, and clean removal.

*Note: Since Tiro is portable, you don't need to "install" anything. You can move the folder anywhere!*

---

## 🛠 Build from Source

Tiro is a plain Win32 C++ project: one `.vcxproj`, one batch file, no CMake.
Step-by-step instructions - toolchain, the model download, the build, and how
to prove the result works - are in **[BUILD.md](BUILD.md)**.

Working with a coding agent? Point it at **[AGENTS.md](AGENTS.md)** - the build
command, the verification gate, the hard rules and a file map, in one page.

---

## 🔒 Privacy Guarantee

Tiro was built with a "Privacy First" philosophy. Because it uses your own computer's power to process your voice, there is **no connection to the internet** required for transcription. Your data is your own.

---

## 📄 Licence

Tiro's own source code is released under the **MIT licence** (see `LICENSE`).
The speech model, the speech engine, the inference runtime and the optional
punctuation model keep their own licences - those texts travel with the
download in `LICENSE.txt` and `NOTICE.txt`.

