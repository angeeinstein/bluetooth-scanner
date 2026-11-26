@echo off
rem Starts the BTScanner Flasher with a real Python install (not the Microsoft
rem Store stub). Keeps this window open if something goes wrong.
cd /d "%~dp0"
set "PY="
rem Prefer a real python.exe (the Microsoft Store stub fails this test), then
rem the py launcher.
python -c "import sys" >nul 2>nul && set "PY=python"
if not defined PY py -3 -c "import sys" >nul 2>nul && set "PY=py -3"
if not defined PY (
  echo Python 3 was not found. Install it from https://www.python.org/downloads/
  echo and tick "Add python.exe to PATH" in the installer, then run this again.
  pause
  exit /b 1
)
%PY% -c "import tkinter" 2>nul || (
  echo This Python has no Tkinter. Reinstall Python from https://www.python.org/downloads/
  echo with the "tcl/tk and IDLE" option enabled.
  pause
  exit /b 1
)
%PY% flasher.pyw
if errorlevel 1 (
  echo.
  echo The flasher closed with an error - see above, or flasher-error.log.
  pause
)
