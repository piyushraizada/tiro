@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  make_release.bat <version>      e.g.   make_release.bat 1.1.1
rem
rem  Turns an already-built, COMMITTED tree into the distributable artifact:
rem    build\Tiro-<version>-win64.zip  (+ .sha256 beside it)
rem
rem  Gates, in order:
rem    1. working tree clean            (else the About stamp says -dirty)
rem    2. tag v<version> exists         (created here if missing; never pushed)
rem    3. exe + the four documents present in bin\Release
rem    4. the model folder matches its model.json
rem    5. zip written with .NET ZipFile (Explorer-valid), folder "Tiro\" inside
rem    6. the finished zip is re-opened and checked: required entries,
rem       and the Tiro.exe inside hashes equal to bin\Release\Tiro.exe
rem    7. SHA-256 written next to the zip
rem
rem  Run build_release.bat first. Nothing is uploaded; publishing is manual.
rem ===========================================================================

set VER=%~1
if "%VER%"=="" (
    echo usage: make_release.bat ^<version^>    e.g.  make_release.bat 1.1.1
    exit /b 2
)

set GITCMD=git
if exist "C:\Program Files\Git\cmd\git.exe" set GITCMD="C:\Program Files\Git\cmd\git.exe"

set MODEL=bin\Release\models\nemotron-0.6b-en
set REQ=bin\Release
set STAGE=build\release
set ROOT=%STAGE%\Tiro
set OUT=build\Tiro-%VER%-win64.zip

rem --- 1. clean tree ---------------------------------------------------------
%GITCMD% status --porcelain --untracked-files=no > "%TEMP%\tiro_status.txt" 2>nul
for %%A in ("%TEMP%\tiro_status.txt") do set STATUSSIZE=%%~zA
del "%TEMP%\tiro_status.txt" >nul 2>&1
if not "%STATUSSIZE%"=="0" (
    echo error: the working tree has uncommitted changes.
    echo        Commit first - otherwise the About box and the zip say -dirty.
    exit /b 1
)
for /f "usebackq delims=" %%h in (`%GITCMD% rev-parse --short HEAD`) do set HASH=%%h
echo commit: !HASH!

rem --- 1b. the version strings agree -----------------------------------------
rem A release whose exe says one version, whose resource says another and whose
rem changelog says a third is a support problem nobody can reproduce. Refuse to
rem tag before the tag exists. The quote character is built with [char]34 so the
rem PowerShell one-liner stays safe inside batch quoting.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$v = '%VER%'; $q = [char]34;" ^
  "$m = Get-Content -Raw 'src\main.cpp';" ^
  "if ($m -notmatch ('kAppVersion\s*=\s*L' + $q + [regex]::Escape($v) + $q)) { Write-Host ('error: src\main.cpp kAppVersion is not ' + $v); exit 1 };" ^
  "$rc = 'FILEVERSION ' + (($v -split '\.') -join ',') + ',0';" ^
  "$r = Get-Content -Raw 'src\Tiro.rc';" ^
  "if (-not $r.Contains($rc)) { Write-Host ('error: src\Tiro.rc has no ' + $rc); exit 1 };" ^
  "if (-not (Select-String -Path 'CHANGELOG.md' -Pattern ('^## ' + [regex]::Escape($v)) -Quiet)) { Write-Host ('error: CHANGELOG.md has no section for ' + $v); exit 1 };" ^
  "Write-Host ('version: ' + $v + ' - exe, resource and changelog agree')"
if errorlevel 1 exit /b 1

rem --- 2. tag ----------------------------------------------------------------
for /f "usebackq delims=" %%h in (`%GITCMD% rev-parse HEAD`) do set HEADFULL=%%h
%GITCMD% rev-parse -q --verify "refs/tags/v%VER%" >nul 2>&1
if errorlevel 1 (
    %GITCMD% tag -a "v%VER%" -m "Tiro v%VER%"
    if errorlevel 1 ( echo error: could not create tag v%VER% & exit /b 1 )
    echo tag:    v%VER% created - not pushed
) else (
    for /f "usebackq delims=" %%t in (`%GITCMD% rev-list -n 1 v%VER%`) do set TAGHASH=%%t
    if not "!TAGHASH!"=="!HEADFULL!" (
        echo error: tag v%VER% points at !TAGHASH! but HEAD is !HEADFULL!.
        echo        Move the tag to this commit before releasing, or bump the version.
        exit /b 1
    )
    echo tag:    v%VER% already exists and matches HEAD
)

rem --- 3. build output present ----------------------------------------------
if not exist "%REQ%\Tiro.exe" (
    echo error: %REQ%\Tiro.exe is missing - run build_release.bat first.
    exit /b 1
)
for %%f in (README.txt LICENSE.txt NOTICE.txt SECURITY.md) do (
    if not exist "%REQ%\%%f" (
        echo error: %REQ%\%%f is missing - run build_release.bat, which copies the docs.
        exit /b 1
    )
)

rem --- 4. model matches its manifest ----------------------------------------
if not exist "%MODEL%\model.json" (
    echo error: %MODEL%\model.json is missing - the package would ship no model.
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$m = Get-Content -Raw '%MODEL%\model.json' | ConvertFrom-Json;" ^
  "foreach ($k in 'encoder','decoder','joiner','tokens') {" ^
  "  $p = Join-Path '%MODEL%' $m.files.$k;" ^
  "  if (-not (Test-Path -LiteralPath $p)) { Write-Host ('missing model file: ' + $p); exit 1 } }"
if errorlevel 1 exit /b 1

rem --- 4b. model files match the shipped hash manifest ----------------------
rem PROVENANCE.md says where the weights come from; this makes that checkable.
rem dist\models.sha256 is committed and travels inside the zip as SHA256SUMS.txt,
rem so anyone - a user, an IT reviewer - can re-hash the 633 MB of weights. A
rem changed weight without a deliberately updated manifest stops the release here,
rem which is the point: the model decides what text gets typed.
if not exist "dist\models.sha256" (
    echo error: dist\models.sha256 is missing - the model has no pinned hashes.
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$bad = 0; $n = 0;" ^
  "foreach ($line in Get-Content '%CD%\dist\models.sha256') {" ^
  "  $line = $line.Trim(); if (-not $line) { continue };" ^
  "  $h, $f = $line -split '\s+', 2; $f = $f.Trim(); $n++;" ^
  "  $p = Join-Path '%MODEL%' $f;" ^
  "  if (-not (Test-Path -LiteralPath $p)) { Write-Host ('FAIL missing model file: ' + $f); $bad = 1; continue };" ^
  "  $a = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower();" ^
  "  if ($a -ne $h.ToLower()) { Write-Host ('FAIL model hash mismatch: ' + $f); $bad = 1 } };" ^
  "if ($n -eq 0) { Write-Host 'FAIL dist\models.sha256 lists no files'; exit 1 };" ^
  "if ($bad) { exit 1 };" ^
  "Write-Host ('model: ' + $n + ' files match dist\models.sha256')"
if errorlevel 1 exit /b 1

rem --- 5. stage and zip -----------------------------------------------------
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%ROOT%\models\nemotron-0.6b-en" 2>nul
copy /y "%REQ%\Tiro.exe"     "%ROOT%\" >nul
copy /y "%REQ%\README.txt"   "%ROOT%\" >nul
copy /y "%REQ%\LICENSE.txt"  "%ROOT%\" >nul
copy /y "%REQ%\NOTICE.txt"   "%ROOT%\" >nul
copy /y "%REQ%\SECURITY.md"  "%ROOT%\" >nul
xcopy /E /I /Y /Q "%MODEL%" "%ROOT%\models\nemotron-0.6b-en" >nul
copy /y "dist\licenses\nemotron\LICENSE.txt" "%ROOT%\models\nemotron-0.6b-en\" >nul
copy /y "dist\licenses\nemotron\NOTICE.txt"  "%ROOT%\models\nemotron-0.6b-en\" >nul
copy /y "dist\models.sha256" "%ROOT%\models\nemotron-0.6b-en\SHA256SUMS.txt" >nul

if exist "%OUT%" del "%OUT%"
echo zipping: about 640 MB, this takes a moment ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
  "try { [IO.Compression.ZipFile]::CreateFromDirectory('%CD%\%STAGE%', '%CD%\%OUT%', [IO.Compression.CompressionLevel]::Optimal, $false) } catch { Write-Host ('zip error: ' + $_.Exception.Message); exit 1 }"
if errorlevel 1 ( echo error: zip creation failed & exit /b 1 )

rem --- 6. verify the finished artifact --------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Add-Type -AssemblyName System.IO.Compression.FileSystem;" ^
  "$out = '%CD%\%OUT%'; $z = [IO.Compression.ZipFile]::OpenRead($out);" ^
  "try {" ^
  "  $names = $z.Entries | ForEach-Object { $_.FullName.Replace('\', '/') };" ^
  "  $need = @('Tiro/Tiro.exe','Tiro/README.txt','Tiro/LICENSE.txt','Tiro/NOTICE.txt','Tiro/SECURITY.md'," ^
  "            'Tiro/models/nemotron-0.6b-en/model.json','Tiro/models/nemotron-0.6b-en/encoder.int8.onnx'," ^
  "            'Tiro/models/nemotron-0.6b-en/SHA256SUMS.txt'," ^
  "            'Tiro/models/nemotron-0.6b-en/LICENSE.txt','Tiro/models/nemotron-0.6b-en/NOTICE.txt');" ^
  "  foreach ($n in $need) { if (-not ($names -contains $n)) { Write-Host ('FAIL missing in zip: ' + $n); exit 1 } }" ^
  "  $e = $z.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq 'Tiro/Tiro.exe' };" ^
  "  $s = $e.Open(); try { $inZip = (Get-FileHash -InputStream $s -Algorithm SHA256).Hash } finally { $s.Dispose() }" ^
  "  $local = (Get-FileHash ('%CD%\bin\Release\Tiro.exe') -Algorithm SHA256).Hash;" ^
  "  if ($inZip -ne $local) { Write-Host 'FAIL Tiro.exe inside the zip differs from the built exe'; exit 1 }" ^
  "  Write-Host ('verified: ' + $z.Entries.Count + ' entries, exe sha256 ' + $inZip.ToLower())" ^
  "} finally { $z.Dispose() }"
if errorlevel 1 ( echo error: artifact verification failed - zip deleted & del "%OUT%" & exit /b 1 )

rem --- 7. checksum ----------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$h = (Get-FileHash ('%CD%\%OUT%') -Algorithm SHA256).Hash.ToLower();" ^
  "Set-Content -Path ('%CD%\%OUT%.sha256') -Value ($h + '  Tiro-%VER%-win64.zip') -Encoding ascii -NoNewline;" ^
  "Write-Host ('sha256:  ' + $h)"
if errorlevel 1 exit /b 1
if not exist "%OUT%.sha256" (
    echo error: the checksum file was not written
    exit /b 1
)

rmdir /s /q "%STAGE%"
for %%A in ("%OUT%") do (
    echo artifact: build\%%~nxA
    echo bytes:    %%~zA
)

echo.
echo next: upload %OUT% and %OUT%.sha256 to the GitHub release for v%VER%.
echo        before that, the release checklist in AGENTS.md:
echo          - README names the asset pattern Tiro-%VER%-win64.zip
echo          - SECURITY.md still describes the binary you just built
echo          - release notes written in the CHANGELOG voice
exit /b 0
