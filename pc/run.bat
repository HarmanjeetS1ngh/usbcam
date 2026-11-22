@echo off
REM Usage: run.bat [width] [height] [fps] [bitrate_bps] [camera_id]   e.g. run.bat 3840 2160 30 40000000
set W=%1
if "%W%"=="" set W=1920
set H=%2
if "%H%"=="" set H=1080
set FPS=%3
if "%FPS%"=="" set FPS=30
set BR=%4
if "%BR%"=="" set BR=12000000
set CAM=%5
set CAMARG=
if not "%CAM%"=="" set CAMARG=--es cam %CAM%

adb forward tcp:5000 tcp:5000
adb shell am start -n com.harman.usbcam/.MainActivity --ei w %W% --ei h %H% --ei fps %FPS% --ei br %BR% %CAMARG%
timeout /t 3 >nul
ffplay -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -framerate %FPS% -f h264 tcp://127.0.0.1:5000
