# Generates the synthetic-voice fixture the app suite feeds to Tiro.
# 16 kHz mono 16-bit PCM, which is what the recogniser expects.
param(
    [string]$Out   = "$PSScriptRoot\..\test\fixtures\voice_suite.wav",
    [string]$Text  = "The quick brown fox jumps over the lazy dog near the river bank.",
    [string]$Voice = ""
)
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ($Voice) { $s.SelectVoice($Voice) }
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
          [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
          [System.Speech.AudioFormat.AudioChannel]::Mono)
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$s.SetOutputToWaveFile($Out, $fmt)
$s.Speak($Text)
$s.Dispose()
$f = Get-Item $Out
"voice: $($s.Voice.VoiceInfo.Name)"
"wrote: $($f.FullName)  $([math]::Round($f.Length/1024,1)) KB"
