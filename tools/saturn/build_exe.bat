@echo off
rem Builds "RE1 Saturn Extractor.exe" - a single program players can run
rem without installing Python. Only the person making the release needs
rem Python 3 (https://www.python.org/downloads/) to run this once.
cd /d "%~dp0"
set PY=
where py >nul 2>nul && set PY=py -3
if not defined PY where python >nul 2>nul && set PY=python
if not defined PY ( echo Python 3 is not installed. & pause & exit /b 1 )
%PY% -m pip install --user numpy pillow pyinstaller || ( pause & exit /b 1 )
set DATA=
for %%f in (battle2pc.py battle_title.py title_menu.py option_tabs.py satbgm.py satcostume.py tick2pc.py ticksnd.py) do call set DATA=%%DATA%% --add-data "%%f;."
%PY% -m PyInstaller --noconfirm --clean --onefile --windowed --name "RE1 Saturn Extractor" --paths . --hidden-import satlz --hidden-import satmodel --hidden-import satsnd --hidden-import satseq --hidden-import tick2pc --hidden-import numpy --hidden-import PIL.Image %DATA% saturn_extractor.py || ( pause & exit /b 1 )
rmdir /s /q build 2>nul
del "RE1 Saturn Extractor.spec" 2>nul
echo.
echo Built: %~dp0dist\RE1 Saturn Extractor.exe
pause
