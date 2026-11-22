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
- [x] M1 code written; builds via GitHub Actions and runs (user confirmed it streams to ffplay). Setup script `setup.ps1` (outside this zip) installs git/gh/adb/ffmpeg, pushes repo, builds, installs APK.
- [x] First test result: ~1 s delay at start, growing to 6-7 s after 30-40 s. Diagnosis: raw H.264 has no timestamps and ffplay assumes 25 fps; stream is 30 fps, so backlog grows ~5 frames/s (fits the numbers). Fix applied in `pc/run.bat` (`-framerate %FPS% -sync ext`). VERIFIED by user: delay is now minimal and stays flat.
- [x] Interim OBS path added: `pc/obs-bridge.bat` (ffmpeg -c copy -> udp://127.0.0.1:5001 MPEG-TS). UNTESTED in OBS.
- [x] S22 result: OBS Media Source over udp = ~600 ms at 1080p30 (ffplay path is near-zero, so the extra delay is OBS Media Source buffering => justifies native plugin M4). Tip: OBS Window Capture of the ffplay window gives ffplay's low delay as a stopgap.
- [x] S22 4K finding: at 4K the stream comes from a DIFFERENT lens than the main camera; user also wants the telephoto selectable (it supports 4K). Fix built (UNTESTED): camera list on phone screen (+ logcat tag UsbCam) and `--es cam <id>` (normal or physical id; physical via OutputConfiguration). Run `pc/run.bat` with 5th arg = cam id.
- [x] S22 camera map (on-screen list): logical cam 0 = {phys 2 ultrawide 2.2mm, phys 5 main 5.4mm (max 4080x3060), phys 6 tele 7.0mm (max 3648x2736)}; cam 2 = ultrawide direct; cam 1 and 3 = front. All report AE fps max 30 => 1080p60/4K60 probably NOT exposed to third-party Camera2 on S22 (unverified; test `run.bat 1920 1080 60 20000000 5`). Tele cannot do 3840x2160 natively (width 3648), so app now snaps to nearest supported same-aspect size (UNTESTED). HW encoders fine: c2.qti.avc to 8192x8192/220 Mbps, hevc 160 Mbps; the camera API, not the encoder, is the limit.
- [ ] (old) User tests OBS: Sources > + > Media Source; untick Local File; Input `udp://127.0.0.1:5001`; Input Format blank; Network Buffering 0; tick hardware decoding. Record resulting delay.
- [ ] Measure real latency (stopwatch method) on BOTH S22 and Realme 7 and record the encoder list each phone shows. (Ask user which phone was used in test 1.)
- [ ] M2 capability-driven mode selection + JSON control channel + foreground service (screen-off) + buffer pool
- [ ] M3 PC receiver (hardware decode, auto-connect on plug-in)
- [ ] M4 OBS source plugin
- [ ] M5 tray/GUI, thermal/battery guard, polish

## Next steps (in order)
0. Rebuild APK (re-run setup.ps1), read the camera list on the S22, test `--es cam <id>` for main/ultrawide/telephoto at 1080p and 4K; record which ids work and the focal length of each.
1. (Realme 7 not available yet; test later) Test obs-bridge.bat in OBS (above) and 4K: `run.bat 3840 2160 30 40000000` on S22 and Realme 7; record which modes work.
2. Initial ~1 s is likely ffplay startup + encoder buffering. Do NOT tune ffplay further: it is a throwaway test player.
3. M2 protocol change: prefix every frame with [4-byte length][8-byte pts_us][1-byte key flag] so the PC can drop stale frames and measure latency. Phone side: try KEY_LATENCY 0, shorter I-frame interval, buffer pool, foreground service, capability-driven modes, JSON control channel.
4. M3 PC receiver renders the NEWEST decoded frame and drops older ones (this is what keeps latency flat).

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
The user prefers direct, honest feedback, and narrow answers over long lists. Tell me when you finish a milestone and give me the updated zip contents."
