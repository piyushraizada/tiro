TIRO
====
Dictation for Windows. Press once, talk, and the words appear wherever your
cursor is.

----------------------------------------------------------------------------
WHAT YOU NEED
----------------------------------------------------------------------------
Windows 10 or Windows 11, 64-bit. Runs on the CPU - no graphics card
(GPU) needed. Nothing else: no installer, no .NET, no
Visual C++ redistributable, no Python, no internet connection.

----------------------------------------------------------------------------
RUNNING IT
----------------------------------------------------------------------------
Unzip this folder anywhere you like - your Desktop, a data drive, a USB stick -
and double-click Tiro.exe. It does not install itself and it does not need
administrator rights.

Keep the models folder next to Tiro.exe. That is where Tiro looks for the
speech model; without it Tiro will say so and refuse to start dictating. The
speech model is included in this download - models\nemotron-0.6b-en - so there
is nothing else to fetch before you start.

Because the file is not code-signed, Windows SmartScreen may warn you the first
time. Choose "More info" then "Run anyway".

----------------------------------------------------------------------------
THE TRAY ICON
----------------------------------------------------------------------------
The icon is the eye: a dark metal ring around a red-orange lens. A coloured
glow along its lower edge tells you the state.

  pale grey arc       still starting (or no usable model): not ready
    circling the eye
  red glow            ready, but not listening
  green glow          listening

----------------------------------------------------------------------------
USING IT
----------------------------------------------------------------------------
Left-click the icon      start or stop dictation. It stays on until you stop
                         it - it hears every word you say.
Right-click the icon     opens the menu:
  Select Microphone        choose which mic Tiro listens to.
  Select Model             choose which speech model Tiro uses. The new
                           choice takes effect the next time you start
                           dictating (not mid-session). Only sherpa-onnx
                           streaming transducer models load - Nemotron
                           ships today; other formats will not work.
  Configure Hotkey         set the keyboard shortcut that starts and
                           stops dictation (the same action as
                           left-clicking the icon).
  Disable Eyeball          tick to hide the eyeball pop-up while you
    Animation              speak; untick to bring it back.
  Disable Punctuation      tick to turn off the optional AI sentence-
                           polish pass described below (applies after a
                           restart). Ticked is the default - out of the box
                           this pass is off.
  Enable at Startup        start Tiro automatically when Windows starts.
  Restart                  relaunch Tiro with your latest settings.
  About                    version, credits and license details.
  Exit                     close Tiro completely.

Press the hotkey         same as a left-click (default Ctrl+Alt+Space, and you
                         can change it in Configure Hotkey)

While dictation is on, the eyeball appears when you speak and fades away five
seconds after you stop. It reappearing means Tiro is hearing you again - it
never took its ears off. Not your style? Right-click the tray icon and check
"Disable Eyeball Animation" - it will never appear again unless you uncheck it.

Text is typed where your cursor is. For punctuation, Tiro decides per
dictation, per application, who does the punctuating. If the application lets
Tiro apply a clean one-shot correction, the speech model's own punctuation is
held back while you dictate and the strict punctuation model rewrites the whole
sentence once, invisibly, when you stop. If the application does not support
that, nothing is hidden or rewritten: text arrives exactly as the speech model
hears it, including its own punctuation ("hello world. it works." arrives with
the period in place), so you always get at least the punctuation the model
supplies.

There is an optional second pass that
re-reads the sentence you just dictated and rewrites punctuation and
capitalisation more strictly ("hello world it works" becomes "Hello world. It
works."). It uses a separate 645 MB model that is NOT part of this download.
To add it: open
https://huggingface.co/Horizon-Labs/punctuation-restoration-base/tree/main,
download config.json, LICENSE, NOTICE and tokenizer.json from that page, then
open the "onnx" folder on the same page and download model_quantized.onnx
(the big one, ~641 MB) - five files in total. Put them in a new folder named
"punctuation" inside the
models folder next to Tiro.exe, and restart. Without those files text is typed
exactly as the model hears it, and everything
else works normally - the difference is only the full-sentence rewrite. And
if the switch is on but the files are missing, Tiro
says exactly that instead of going quiet.
It is OFF by default. To try it: right-click the tray icon,
un-tick "Disable Punctuation", and restart Tiro - it will confirm that on
the spot, because it never interrupts a dictation mid-sentence. When armed,
the moment you stop dictating, Tiro turns the words you just spoke into
their finished form in one seamless step: no clipboard, nothing visibly
selected, and the cursor stays right where you left it so you can keep
dictating. Where an application does not accept that seamless replacement,
your text keeps the punctuation you spoke as-is, and Tiro still closes the
sentence for you. Un-ticking the menu item is not needed - ticking
"Disable Punctuation" again turns the pass off, from the next start.

Notifications (an error, an instruction) appear for five seconds and dismiss
themselves.

----------------------------------------------------------------------------
MODELS
----------------------------------------------------------------------------
models\ holds one folder per speech model, each with a model.json and its
files. The default is models\nemotron-0.6b-en. A second folder,
models\punctuation, is never required - only the optional second-pass model
described above lives there.

To add a model, copy its folder into models\, then right-click the tray icon
and pick it under "Select Model"; the new model is loaded the next time you
start dictating. The executable never changes. Note that
only sherpa-onnx streaming transducer models are supported - Nemotron ships
today, and other model formats will not load.

Models are third-party work with their own licences - see NOTICE.txt.

----------------------------------------------------------------------------
WHERE IT KEEPS THINGS
----------------------------------------------------------------------------
  HKEY_CURRENT_USER\Software\Tiro\   your microphone, model and hotkey choices
  %LOCALAPPDATA%\Tiro\logs\          the log file
  %APPDATA%\Microsoft\Windows\
     Start Menu\Programs\Startup\    one Tiro.lnk shortcut, and only if you
                                     checked Enable at Startup

Everything lives inside your own user profile - no administrator rights are
needed anywhere, ever. Nothing is written next to Tiro.exe. By default no
audio and no transcript is saved to disk or sent anywhere: the log holds
events and error codes only. Two diagnostic switches exist for development
and are off unless someone deliberately sets them (see SECURITY.md): LogText
adds the dictated words to the log, RecordWav saves the microphone audio as
WAV files in the same logs folder. Everything runs on your own CPU.

----------------------------------------------------------------------------
REMOVING IT
----------------------------------------------------------------------------
There is no installer and no uninstaller - Tiro was never "installed".
Right-click the tray icon and choose Exit. Then delete this folder, the
%LOCALAPPDATA%\Tiro folder, and, if you ever checked Enable at Startup,
Tiro.lnk from the Startup folder listed above. The settings key above can
be removed too; if you do not, the next person who runs a Tiro.exe simply
inherits your microphone and hotkey choices.

----------------------------------------------------------------------------
IF SOMETHING GOES WRONG
----------------------------------------------------------------------------
"Transcription engine failed      models\ is missing or holds no usable model.
 to load. See the log file."      The icon stays on the pale arc; fix the
                                  folder, then restart Tiro.

"Could not open the microphone   another program is holding the microphone
 - it may be in use by another   exclusively. Close it and try again.
 application."

"No microphones available to      Windows sees no recording device.
 transcribe."

Tiro turns itself off and tells     the microphone was unplugged, or you
you why, rather than going quiet   clicked into another window. Turn it back
                                   on when you are ready.
The words trail behind your        the CPU is too slow for the model. Tiro
voice, or the odd word goes        drops the oldest audio to keep up and
missing                            records that in the log; close other heavy
                                   programs, or see "Select Microphone" and
                                   pick a device closer to your mouth.

The log file is %LOCALAPPDATA%\Tiro\logs\app.log - it records events and error
codes. It never contains your words unless the LogText diagnostic switch is
turned on (off by default). When it passes 2 MB it is moved aside to
app.log.old at the next start, so it never grows without bound.

----------------------------------------------------------------------------
DIAGNOSTICS (FOR SUPPORT)
----------------------------------------------------------------------------
All diagnostic commands sit behind one switch: --debug. Without it they do
nothing at all, and the log says so; the normal double-click never uses it.

  Tiro.exe --debug                 a diagnostic copy of Tiro. It does NOT take
                                   over from a running Tiro - your tray copy
                                   keeps working and this second copy just
                                   logs - so a test never interrupts dictation.

Each command below does its job and exits, writing its result to the same log
file. They are meant for whoever is helping you, not for daily use.

  --debug --about                  opens the About box
  --debug --hotkey                 opens Configure Hotkey
  --debug --notify                 shows a test notification
  --debug --startup-toggle         turns "Enable at Startup" on or off
  --debug --restart                runs the same restart as the tray menu
  --debug --selftest <file.wav>    plays a 16 kHz mono 16-bit WAV through the
                                   speech engine and logs what it heard; the
                                   microphone is not used
  --debug --live <file.wav>        feeds that WAV through the live typing path
                                   (with --target "<window title>" to name the
                                   window; it must still have focus)
  --debug --rewrite-demo           checks the optional punctuation pass end to
                                   end against a window Tiro creates itself
  --debug --polish-file <in> <out> rewrites a UTF-8 text file with the
                                   optional punctuation model, if installed
  --debug --polish-tokens <in> <out>
                                   dumps how that model splits the file into
                                   tokens; tokenizer investigations only

What a --debug run changes, in full: the single-instance rule is not enforced
(so it can run beside your tray copy), the commands above are accepted, and it
logs one extra line saying so. It does not change how dictation behaves, does
not record the microphone, does not put your words in the log (that is the
LogText switch, off by default) and never opens a network connection.

----------------------------------------------------------------------------
FOR YOUR IT OR SECURITY TEAM
----------------------------------------------------------------------------
Tiro is free software that is not code-signed, so SmartScreen will warn about
an "unverified publisher" on a freshly downloaded copy: "More info" then "Run
anyway" is the path, once anyone is comfortable. Antivirus may ask why an app
touches the microphone and types into other windows - that is exactly what a
dictation app does. SECURITY.md, next to this file, answers the full list of
reviewer questions: every sensitive API and when it runs, the explicit list of
what Tiro never does (it opens no network connection at all, uses no keyboard
hooks, injects no code), and the exact third-party components inside the
executable with their versions. If an exception ticket needs a publisher and
file-path scope, that document has it.

----------------------------------------------------------------------------
LICENCES
----------------------------------------------------------------------------
The default speech model is NVIDIA Nemotron 0.6B, used under the NVIDIA Open
Model License: "Licensed by NVIDIA Corporation under the NVIDIA Open Model
License". The punctuation model is Horizon-Labs punctuation-restoration-base
(Apache 2.0), a fine-tune of the MIT-licensed mmBERT-base; it is an optional
download and not shipped in this package, so those terms reach you only if
you add it. The recognition
engine is sherpa-onnx (Apache 2.0) running on ONNX Runtime (MIT). Full licence
texts are in LICENSE.txt; attribution is in NOTICE.txt. Keep LICENSE.txt,
NOTICE.txt and the models folders together with Tiro.exe - the licences
travel with the files.
