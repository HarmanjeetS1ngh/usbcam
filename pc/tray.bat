@echo off
rem Starts the UsbCam tray icon (no console window). Needs adb on PATH.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tray.ps1"
