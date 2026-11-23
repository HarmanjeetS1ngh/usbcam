// Shared between the camera DLL (runs inside Windows' Frame Server, LocalService, session 0)
// and the feeder program usbcam_vcam.exe (runs as the user). Plain C++, no dependencies.
#pragma once
#include <windows.h>
#include <stdint.h>

// {6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}  CLSID of the media source (COM class in usbcam_vcam_src.dll)
static const GUID CLSID_UsbCamSource = { 0x6b1f5d3a, 0x2c47, 0x4e58, { 0x9a, 0x0b, 0x3d, 0x7e, 0x41, 0xc8, 0xa5, 0xf2 } };
#define UCV_CLSID_STR   L"{6B1F5D3A-2C47-4E58-9A0B-3D7E41C8A5F2}"
#define UCV_CAMERA_NAME L"UsbCam Camera"

// The DLL creates these (it runs as a service and may create Global objects); the feeder only opens them.
// Fallback names in the user's own session are tried too (in case Frame Server runs the source as the user).
#define UCV_MAP_GLOBAL   L"Global\\UsbCamVcamFrames"
#define UCV_EVT_GLOBAL   L"Global\\UsbCamVcamFrameEvent"
#define UCV_MAP_LOCAL    L"Local\\UsbCamVcamFrames"
#define UCV_EVT_LOCAL    L"Local\\UsbCamVcamFrameEvent"

#define UCV_MAGIC     0x4D564355u   // 'UCVM'
#define UCV_VERSION   1u
#define UCV_SLOTS     4
#define UCV_MAX_W     1920
#define UCV_MAX_H     1080
#define UCV_HDR_BYTES 4096u
// one slot = one NV12 frame at most 1920x1080, rounded up to 4096
#define UCV_SLOT_BYTES (((UCV_MAX_W * UCV_MAX_H * 3 / 2) + 4095u) & ~4095u)
#define UCV_TOTAL_BYTES (UCV_HDR_BYTES + UCV_SLOTS * UCV_SLOT_BYTES)

struct UcvSlotInfo {          // lives in the header, one per slot
    uint32_t w, h;            // NV12 size of the frame in this slot
    uint32_t live;            // 1 = real phone picture, 0 = "Waiting for phone" placeholder
    uint32_t pad;
    int64_t  t_written_us;    // QueryPerformanceCounter in microseconds when the feeder finished writing
};

struct UcvHeader {
    uint32_t magic, version;
    volatile LONG64 seq;      // number of frames published so far; the newest frame is in slot (seq % UCV_SLOTS)
    // set by the source (DLL):
    volatile LONG want_w, want_h;     // size the apps currently ask for (0 = nothing yet)
    volatile LONG64 src_tick_ms;      // QPC ms, refreshed by the source thread while an app has the camera started
    volatile LONG64 src_frames;       // frames handed to Frame Server
    volatile LONG64 src_lat_sum_us;   // sum of (delivery time - slot written time), for the log
    volatile LONG64 src_lat_max_us;
    volatile LONG64 src_dups;         // repeated frames (nothing new to send)
    volatile LONG src_pid;
    // set by the feeder:
    volatile LONG mirror;             // 1 = mirror the picture (done by the feeder while scaling)
    volatile LONG feeder_pid;
    UcvSlotInfo slot[UCV_SLOTS];
};
static_assert(sizeof(UcvHeader) < UCV_HDR_BYTES, "header too big");

static inline int64_t UcvNowUs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}
static inline uint8_t* UcvSlotPtr(void* base, int64_t seq) {
    return (uint8_t*)base + UCV_HDR_BYTES + (size_t)(seq % UCV_SLOTS) * UCV_SLOT_BYTES;
}
