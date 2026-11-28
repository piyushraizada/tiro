<#
  App suite - the cross-application regression run.

  Two passes over each application, every time:
    basic   polish off. The statement under test is the product promise: speech lands in the
            writeable area, as typed, with no polish. A failure here is a release blocker.
    polish  polish on. Recorded, not fatal: the point is the inventory of which application
            can take the one-shot rewrite, so a refusal is data, not a crash.

  Preconditions the operator owns:
    - each application is running, with its window title containing the token below
    - the caret is inside a text area in each one (a scratch document, an empty compose box)
    - nobody touches the machine while it runs: the suite brings each window forward and types

  Usage:
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-app-suite.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-app-suite.ps1 -Apps Word,Teams
    powershell -NoProfile -ExecutionPolicy Bypass -File tests\run-app-suite.ps1 -PreflightOnly

  Exit code: 0 when every basic run passed, 1 when any did not, 2 when setup is wrong.
#>
param(
    [string] $Exe        = (Join-Path $PSScriptRoot '..\bin\Release\Tiro.exe'),
    [string] $Wav        = (Join-Path $PSScriptRoot '..\test\fixtures\voice_suite.wav'),
    [string] $Expect     = 'The quick brown fox jumps over the lazy dog near the river bank.',
    [string[]] $Apps     = @('Word', 'Teams', 'Outlook', 'Notepad++', 'VS Code'),
    [int]    $TimeoutSec = 120,
    [switch] $PreflightOnly,
    [string] $Out        = (Join-Path $PSScriptRoot '..\test\results')
)

$ErrorActionPreference = 'Stop'
# -File passes a list as one string, so accept "Word,Teams" as well as separate values.
$Apps = @($Apps | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$sets = @{
    'Word'      = @{ Match = @('Word');                  Where = 'document body' }
    'Teams'     = @{ Match = @('Teams');                 Where = 'chat compose box' }
    'Outlook'   = @{ Match = 'Outlook';               Where = 'mail body (Edge)' }
    'Notepad++' = @{ Match = @('Notepad++');             Where = 'editor pane' }
    'VS Code'   = @{ Match = @('Visual Studio Code');    Where = 'editor pane' }
}

Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class TiroSuiteWindows {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    public static List<string> Visible() {
        var list = new List<string>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var t = new StringBuilder(512);
            GetWindowTextW(h, t, 512);
            if (t.Length > 0) list.Add(t.ToString());
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

# Same rule Tiro uses to pin a target: a visible top-level window whose title contains the token.
# The first token in the list that matches wins, so a compose window ("New mail") is preferred to
# the inbox that happens to be open behind it.
function Find-Window([string[]] $tokens) {
    $visible = [TiroSuiteWindows]::Visible()
    foreach ($token in $tokens) {
        $hit = $visible | Where-Object { $_.Contains($token) } | Select-Object -First 1
        if ($hit) { return [pscustomobject]@{ Token = $token; Title = $hit } }
    }
    return $null
}

function Set-Polish([int] $v) {
    New-ItemProperty -Path 'HKCU:\Software\Tiro' -Name Polish -Value $v -PropertyType DWord -Force |
        Out-Null
}

# ---- setup ------------------------------------------------------------------
if (-not (Test-Path $Exe)) { Write-Host "SETUP: no Tiro.exe at $Exe"; exit 2 }
if (-not (Test-Path $Wav)) {
    Write-Host "fixture missing: generating it with the Windows voice"
    & (Join-Path $PSScriptRoot 'make-voice.ps1') -Out $Wav | Out-Null
    if (-not (Test-Path $Wav)) { Write-Host "SETUP: could not generate $Wav"; exit 2 }
}
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

$hadPolish = $false; $oldPolish = 0
try {
    $oldPolish = (Get-ItemProperty -Path 'HKCU:\Software\Tiro' -Name Polish -ErrorAction Stop).Polish
    $hadPolish = $true
} catch { }

Write-Host ''
Write-Host 'Application        Running as                                  Text area'
Write-Host '-------------------------------------------------------------------------------'
$runList = @()
foreach ($name in $Apps) {
    $set = $sets[$name]
    if (-not $set) { Write-Host ("{0,-18} {1}" -f $name, 'unknown application name'); exit 2 }
    $found = Find-Window $set.Match
    if ($found) {
        Write-Host ("{0,-18} {1,-43} {2}" -f $name, $found.Title.Substring(0, [Math]::Min(43, $found.Title.Length)), $set.Where)
        $runList += @{ Name = $name; Match = $found.Token; Title = $found.Title }
    } else {
        Write-Host ("{0,-18} {1}" -f $name, 'NOT RUNNING - skipped')
    }
}
Write-Host ''
if ($runList.Count -eq 0) { Write-Host 'SETUP: none of the applications are running'; exit 2 }
if ($PreflightOnly) { Write-Host 'Preflight only: nothing was typed.'; exit 0 }

# ---- runs -------------------------------------------------------------------
$results = @()
foreach ($app in $runList) {
    foreach ($mode in @('basic', 'polish')) {
        if ($mode -eq 'polish') { Set-Polish 1 } else { Set-Polish 0 }
        $tmp = Join-Path $env:TEMP ("tiro-suite-{0}-{1}.txt" -f ($app.Name -replace '[^A-Za-z]',''), $mode)
        if (Test-Path $tmp) { Remove-Item $tmp -Force }
        $cli = @('--debug', '--live', $Wav, '--target', $app.Match, '--focus', '--stop',
                 '--expect', $Expect, '--mode', $mode, '--result', $tmp)
        $t0 = Get-Date
        $p = Start-Process -FilePath $Exe -ArgumentList $cli -PassThru -WindowStyle Hidden
        $verdict = 'FAIL_TIMEOUT'; $pct = 0; $text = ''
        if ($p.WaitForExit($TimeoutSec * 1000)) {
            if (Test-Path $tmp) {
                $line = (Get-Content -Raw -Path $tmp).Trim()
                $parts = $line -split '\|', 4
                if ($parts.Count -ge 4) { $verdict = $parts[1]; $pct = [int]$parts[2]; $text = $parts[3] }
            }
        } else {
            try { $p.Kill() } catch { }
        }
        $secs = [int]((Get-Date) - $t0).TotalSeconds
        $results += [pscustomobject]@{
            App = $app.Name; Mode = $mode; Verdict = $verdict; Match = $pct; Seconds = $secs; Text = $text
        }
        Write-Host ("{0,-18} {1,-7} {2,-14} {3,3}%  {4,3}s  {5}" -f $app.Name, $mode, $verdict, $pct, $secs, $text)
    }
}

# ---- restore the setting the run borrowed -----------------------------------
if ($hadPolish) { Set-Polish $oldPolish }
else { Remove-ItemProperty -Path 'HKCU:\Software\Tiro' -Name Polish -ErrorAction SilentlyContinue }

# ---- report -----------------------------------------------------------------
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$md = Join-Path $Out ("app-suite-{0}.md" -f $stamp)
$lines = @()
$lines += "# App suite $stamp"
$lines += ''
$lines += "Fixture: synthetic voice, `"$Expect`""
$lines += ''
$lines += '| Application | Basic dictation | Polish pass | What landed |'
$lines += '|---|---|---|---|'
foreach ($app in $runList) {
    $b = $results | Where-Object { $_.App -eq $app.Name -and $_.Mode -eq 'basic' }
    $p = $results | Where-Object { $_.App -eq $app.Name -and $_.Mode -eq 'polish' }
    $cell = ($p.Text) -replace '\|', '/'
    if ($cell.Length -gt 160) { $cell = '...' + $cell.Substring($cell.Length - 160) }
    $lines += ("| {0} | {1} | {2} | {3} |" -f $app.Name, $b.Verdict, $p.Verdict, $cell)
}
$lines += ''
$lines += 'Basic pass is mandatory; a polish failure is recorded as the inventory of what the'
$lines += 'application can accept today.'
$lines | Set-Content -Path $md -Encoding UTF8
Copy-Item $md (Join-Path $Out 'app-suite-latest.md') -Force
Write-Host ''
Write-Host "Report: $md"

$soft = $results | Where-Object { $_.Mode -eq 'basic' -and $_.Verdict -eq 'PASS_DELIVERED' }
foreach ($s in $soft) {
    Write-Host ("NOTE: {0} received the keystrokes but its text is not readable by the harness -" -f $s.App)
    Write-Host "      put the caret inside a text area in that application and run it again."
}
$bad = $results | Where-Object { $_.Mode -eq 'basic' -and $_.Verdict -ne 'PASS' }
if ($bad) { Write-Host ("RESULT: {0} basic run(s) did not pass cleanly" -f $bad.Count); exit 1 }
Write-Host 'RESULT: every basic run passed'
exit 0
