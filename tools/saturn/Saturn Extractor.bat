@echo off
rem RE1 Saturn Extractor - double-click to run.
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set PY=py -3
if not defined PY where python >nul 2>nul && set PY=python
if not defined PY (
  echo Python 3 is not installed. Get it from https://www.python.org/downloads/
  echo ^(tick "Add python.exe to PATH" in the installer^), then run this again.
  pause
  exit /b 1
)
%PY% -c "import numpy, PIL" >nul 2>nul || (
  echo Installing numpy and Pillow ^(first run only^)...
  %PY% -m pip install --user numpy pillow || ( pause & exit /b 1 )
)
%PY% saturn_extractor.py
if errorlevel 1 pause
