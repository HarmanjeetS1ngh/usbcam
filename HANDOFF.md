# UsbCam handoff (update at the end of every milestone)

## Goal
DroidCam-style Android app: phone camera -> PC over USB, lowest latency, native resolution
(up to 4K60 where the phone's hardware encoder allows), controllable from PC, appears in OBS 33.0.3.
Test phones: Samsung Galaxy S22, Realme 7 (old, likely no 4K60 HW encode). Must be light on phone and PC.
PC: Windows, Ryzen 5 5600H, RX 5500M, VS Code only (no Visual Studio). Builds via GitHub Actions.

## Decisions (do not re-debate)
- Transport: ADB over USB. Phone app listens on 127.0.0.1:5000; PC uses `adb forward tcp:5000 tcp:5000`.
- Phone: Kotlin, no dependencies, Camera2 -> MediaCodec hardware H.264, Surface input, no preview.
- Stream: raw Annex-B H.264 over TCP (SPS/PPS prepended to every IDR). Small queue, drop + resync on keyframe.
- PC (M3): hardware decode via D3D11VA (AMD VCN). OBS (M4): native OBS source plugin first
  (build from obs-plugintemplate on GitHub Actions); virtual camera driver later, if wanted.
- Modes are never hardcoded: query MediaCodecList / Camera2 per device, offer only what is supported.
- "Zero latency" is not literal; target < 80 ms glass-to-glass.

## Status
- [x] M1 code written (UNTESTED, not compiled by the author): phone streams H.264, `pc/run.bat` plays it in ffplay.
- [ ] M1 verified on S22 and Realme 7 (build via Actions, install, measure latency)
- [ ] M2 capability-driven mode selection + JSON control channel + foreground service (screen-off) + buffer pool
- [ ] M3 PC receiver (hardware decode, auto-connect on plug-in)
- [ ] M4 OBS source plugin
- [ ] M5 tray/GUI, thermal/battery guard, polish

## Known gaps / notes
- Stream is sensor-oriented (landscape); phone locked to landscape. Rotation handled later.
- CBR/KEY_LATENCY may be rejected by some encoders: add fallbacks in M2 if configure() throws.
- Camera permission must be granted once manually on first launch.

## Run M1
1. Push to GitHub, run the "build-apk" Action, download the APK artifact, `adb install -r app-debug.apk`.
2. Phone: Settings > Developer options > USB debugging on. Open UsbCam once, grant camera.
3. PC: install ffmpeg (ffplay on PATH) + Android platform-tools (adb on PATH). `pc\run.bat 1920 1080 30 12000000`.

## Starter prompt for the next account
"I'm continuing a project in progress: UsbCam, an Android USB webcam app for OBS. Read HANDOFF.md in the
attached zip fully before doing anything; it holds the goal, fixed decisions and current status.
Continue from the first unchecked item. Be credit-efficient: no re-explaining decisions, change only
the files needed (give diffs or single files, not full rewrites), and update HANDOFF.md status when done.
Tell me when you finish a milestone and give me the updated zip contents."
