@echo off
rem ============================================================================
rem  Automated rewrite tests. No interaction, no manual clicking.
rem
rem  Usage:  run-rewrite-tests.bat [path\to\Tiro.exe]
rem  Default exe: ..\build\verify\Tiro.exe
rem
rem  Each test starts its own target window, types into it, runs the real
rem  rewrite code, reads the result back and returns a process exit code.
rem  Exit status: 0 = all passed, 1 = a test failed, 2 = setup problem.
rem ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set "EXE=%~1"
if "%EXE%"=="" set "EXE=..\build\verify\Tiro.exe"
if not exist "%EXE%" ( echo SETUP: Tiro.exe not found at "%EXE%" & exit /b 2 )

set "CHROME="
for %%P in (
  "%ProgramFiles%\Google\Chrome\Application\chrome.exe"
  "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
  "%LocalAppData%\Google\Chrome\Application\chrome.exe"
) do if not defined CHROME if exist %%P set "CHROME=%%~P"
if not defined CHROME ( echo SETUP: Chrome not found & exit /b 2 )

set "FAILED=0"
set "PROFILE=%TEMP%\tiro-uia-test-profile"
set "PAGE=file:///%~dp0uia-target.html"

echo.
echo === 1/2  classic edit control (RichEdit50W) ===============================
"%EXE%" --debug --rewrite-demo
if errorlevel 1 ( echo TEST 1: FAIL & set "FAILED=1" ) else ( echo TEST 1: PASS )

echo.
echo === 2/3  Chromium textarea (UIA ValuePattern) =============================
start "" "%CHROME%" --user-data-dir="%PROFILE%" --no-first-run --no-default-browser-check ^
  --disable-sync --disable-features=Translate --window-size=920,560 --window-position=300,150 ^
  --app="%PAGE%?mode=ta"
rem The page focuses its own control; give the cold profile time to paint.
ping -n 6 127.0.0.1 >nul
"%EXE%" --debug --uia-rewrite
if errorlevel 1 ( echo TEST 2: FAIL & set "FAILED=1" ) else ( echo TEST 2: PASS )

echo.
echo === 3/3  Chromium contenteditable (must stay exactly as typed) ============
start "" "%CHROME%" --user-data-dir="%PROFILE%" --no-first-run --no-default-browser-check ^
  --disable-sync --disable-features=Translate --window-size=920,560 --window-position=300,150 ^
  --app="%PAGE%?mode=ce"
ping -n 6 127.0.0.1 >nul
set "TIRO_UIA_EXPECT=raw"
"%EXE%" --debug --uia-rewrite
set "TIRO_UIA_EXPECT="
if errorlevel 1 ( echo TEST 3: FAIL & set "FAILED=1" ) else ( echo TEST 3: PASS )

echo.
if "%FAILED%"=="1" ( echo RESULT: FAILED & exit /b 1 )
echo RESULT: all rewrite tests passed
exit /b 0
