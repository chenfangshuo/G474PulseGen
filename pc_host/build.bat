@echo off
REM ============================================================
REM  oled_mirror.py packaging script (PyInstaller onefile + no console)
REM  Usage: double-click this file, or run build.bat in cmd
REM  Output: pc_host\dist\OLED_Mirror.exe
REM ============================================================
cd /d "%~dp0"

python -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --onefile ^
  --windowed ^
  --name OLED_Mirror ^
  --hidden-import serial ^
  --hidden-import serial.tools.list_ports ^
  --hidden-import serial.tools.list_ports_common ^
  --hidden-import serial.tools.list_ports_windows ^
  oled_mirror.py

echo.
echo ============================================================
echo  Build done. exe is at: %~dp0dist\OLED_Mirror.exe
echo ============================================================
pause
