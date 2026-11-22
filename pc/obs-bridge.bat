@echo off
REM Interim OBS path (no plugin): phone -> adb -> ffmpeg (-c copy, no decode, ~0% CPU) -> local UDP MPEG-TS -> OBS Media Source.
REM Usage: obs-bridge.bat [width] [height] [fps] [bitrate_bps] [camera_id]
REM Close run.bat first: the phone accepts one PC client at a time.
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

:loop
adb forward tcp:5000 tcp:5000
adb shell am start -n com.harman.usbcam/.MainActivity --ei w %W% --ei h %H% --ei fps %FPS% --ei br %BR% %CAMARG%
timeout /t 3 >nul
ffmpeg -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -use_wallclock_as_timestamps 1 -framerate %FPS% -f h264 -i tcp://127.0.0.1:5000 -c copy -f mpegts -flush_packets 1 -muxdelay 0 "udp://127.0.0.1:5001?pkt_size=1316"
echo Disconnected - retrying in 2 s (Ctrl+C to quit)
timeout /t 2 >nul
goto loop
