@echo off
REM ============================================================
REM  oled_mirror.py 打包脚本 (PyInstaller 单文件 + 无控制台)
REM  用法: 双击本文件, 或在 cmd 下运行 build.bat
REM  产物: pc_host\dist\OLED_Mirror.exe
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
echo  打包完成, exe 位于: %~dp0dist\OLED_Mirror.exe
echo ============================================================
pause
