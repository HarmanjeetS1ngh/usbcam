@echo off
REM Saves the phone app's log (everything tagged UsbCam) to a file and prints it.
REM Usage: log.bat [output_file]   (default: pc\logs\last.txt)
if not exist "%~dp0logs" mkdir "%~dp0logs"
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0logs\last.txt
adb logcat -d -v time -s UsbCam:I > "%OUT%"
echo.
echo ===== log saved to %OUT% =====
type "%OUT%"
