// usbcam_vcam_src.dll: Media Foundation custom media source for the "UsbCam Camera" virtual camera.
// Loaded by Windows' Frame Server (LocalService). Reads the newest frame the feeder (usbcam_vcam.exe) wrote into shared
// memory and hands it to Frame Server as NV12 / YUY2 / RGB32. Original code (no OBS / GPL code).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <new>
#include <vector>
#include <string.h>
#include "vcam_shared.h"

static HMODULE g_mod = nullptr;
static volatile LONG g_objects = 0;

// stream category "capture" (PINNAME_CAPTURE), written out so we do not depend on ksmedia.h GUID definitions
// IID of IKsControl (not defined by the headers we link)
static const GUID kIID_IKsControl = { 0x28f54685, 0x06fd, 0x11d2, { 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID kPinCapture = { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };

enum Fmt { F_NV12 = 0, F_YUY2 = 1, F_RGB32 = 2 };
struct ModeDef { Fmt f; UINT w, h; };
static const ModeDef kModes[] = {
    { F_NV12, 1280, 720 }, { F_NV12, 1920, 1080 },
    { F_YUY2, 1280, 720 }, { F_YUY2, 1920, 1080 },
    { F_RGB32, 1280, 720 }, { F_RGB32, 1920, 1080 },
};
static const int kModeCount = (int)(sizeof(kModes) / sizeof(kModes[0]));

static size_t FrameBytes(Fmt f, UINT w, UINT h) {
    return f == F_NV12 ? (size_t)w * h * 3 / 2 : f == F_YUY2 ? (size_t)w * h * 2 : (size_t)w * h * 4;
}

static HRESULT MakeType(const ModeDef& m, IMFMediaType** out) {
    IMFMediaType* t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (FAILED(hr)) return hr;
    const GUID& sub = m.f == F_NV12 ? MFVideoFormat_NV12 : m.f == F_YUY2 ? MFVideoFormat_YUY2 : MFVideoFormat_RGB32;
    UINT32 stride = m.f == F_NV12 ? m.w : m.f == F_YUY2 ? m.w * 2 : m.w * 4;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, m.w, m.h);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MIN, 30, 1);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE_RANGE_MAX, 30, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
    t->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)FrameBytes(m.f, m.w, m.h));
    if (m.f != F_RGB32) {
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    }
    *out = t;
    return S_OK;
}

// ---------------- pixel conversion (source is always BT.709 limited-range NV12 with stride = width) ----------------
static inline uint8_t Clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void ConvertFrame(Fmt f, const uint8_t* src, UINT w, UINT h, uint8_t* dst) {
    const uint8_t* Y = src;
    const uint8_t* UV = src + (size_t)w * h;
    if (f == F_NV12) { memcpy(dst, src, (size_t)w * h * 3 / 2); return; }
    if (f == F_YUY2) {
        for (UINT y = 0; y < h; y++) {
            const uint8_t* yr = Y + (size_t)y * w;
            const uint8_t* uv = UV + (size_t)(y / 2) * w;
            uint8_t* o = dst + (size_t)y * w * 2;
            for (UINT x = 0; x < w; x += 2) {
                o[0] = yr[x]; o[1] = uv[x]; o[2] = yr[x + 1]; o[3] = uv[x + 1];
                o += 4;
            }
        }
        return;
    }
    for (UINT y = 0; y < h; y++) {
        const uint8_t* yr = Y + (size_t)y * w;
        const uint8_t* uv = UV + (size_t)(y / 2) * w;
        uint8_t* o = dst + (size_t)y * w * 4;
        for (UINT x = 0; x < w; x += 2) {
            int d = (int)uv[x] - 128, e = (int)uv[x + 1] - 128;
            int rr = 459 * e, gg = -55 * d - 136 * e, bb = 541 * d;
            for (int k = 0; k < 2; k++) {
                int c = 298 * ((int)yr[x + k] - 16) + 128;
                o[0] = Clamp8((c + bb) >> 8);
                o[1] = Clamp8((c + gg) >> 8);
                o[2] = Clamp8((c + rr) >> 8);
                o[3] = 255;
                o += 4;
            }
        }
    }
}

// ---------------- shared memory (created here, opened by the feeder) ----------------
struct Shared {
    HANDLE map = nullptr, evt = nullptr;
    UcvHeader* hdr = nullptr;
    uint8_t* base = nullptr;
    bool Open() {
        if (hdr) return true;
        PSECURITY_DESCRIPTOR psd = nullptr;
        // everyone may read/write (the feeder runs as the user); Low integrity label so a normal user process may write
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)(A;;GA;;;AC)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &psd, nullptr)) psd = nullptr;
        SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof sa; sa.lpSecurityDescriptor = psd; sa.bInheritHandle = FALSE;
        const wchar_t* names[2][2] = { { UCV_MAP_GLOBAL, UCV_EVT_GLOBAL }, { UCV_MAP_LOCAL, UCV_EVT_LOCAL } };
        for (int i = 0; i < 2 && !hdr; i++) {
            HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, psd ? &sa : nullptr, PAGE_READWRITE, 0, UCV_TOTAL_BYTES, names[i][0]);
            if (!m) continue;
            void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (!v) { CloseHandle(m); continue; }
            HANDLE e = CreateEventW(psd ? &sa : nullptr, FALSE, FALSE, names[i][1]);
            map = m; evt = e; base = (uint8_t*)v; hdr = (UcvHeader*)v;
            if (hdr->magic != UCV_MAGIC) { hdr->version = UCV_VERSION; hdr->magic = UCV_MAGIC; }
        }
        if (psd) LocalFree(psd);
        if (hdr) hdr->src_pid = (LONG)GetCurrentProcessId();
        return hdr != nullptr;
    }
    void Close() {
        if (base) UnmapViewOfFile(base);
        if (map) CloseHandle(map);
        if (evt) CloseHandle(evt);
        base = nullptr; hdr = nullptr; map = evt = nullptr;
    }
};

// ---------------- the stream ----------------
class UsbCamSource;

class UsbCamStream : public IMFMediaStream2 {
public:
    UsbCamStream(UsbCamSource* src, IMFStreamDescriptor* sd) : m_src(src), m_sd(sd) {
        InterlockedIncrement(&g_objects);
        InitializeCriticalSection(&m_cs);
        MFCreateEventQueue(&m_q);
        m_hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_hReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_sd->AddRef();
    }
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
            *ppv = static_cast<IMFMediaStream2*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return (ULONG)r; }
    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override { return m_q ? m_q->GetEvent(f, e) : MF_E_SHUTDOWN; }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return m_q ? m_q->BeginGetEvent(cb, st) : MF_E_SHUTDOWN; }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return m_q ? m_q->EndGetEvent(r, e) : MF_E_SHUTDOWN; }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override { return m_q ? m_q->QueueEventParamVar(t, ext, hr, v) : MF_E_SHUTDOWN; }
    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppSrc) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** pp) override {
        if (!pp) return E_POINTER; *pp = m_sd; m_sd->AddRef(); return S_OK;
    }
    STDMETHODIMP RequestSample(IUnknown* token) override {
        if (m_shut) return MF_E_SHUTDOWN;
        EnterCriticalSection(&m_cs);
        if (token) token->AddRef();
        m_tokens.push_back(token);
        LeaveCriticalSection(&m_cs);
        SetEvent(m_hReq);
        return S_OK;
    }
    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE) override { return S_OK; }
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* s) override { if (!s) return E_POINTER; *s = m_running ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED; return S_OK; }

    void Begin(Fmt f, UINT w, UINT h) {
        m_fmt = f; m_w = w; m_h = h;
        if (!m_thread) {
            ResetEvent(m_hStop);
            m_thread = CreateThread(nullptr, 0, &UsbCamStream::ThreadMain, this, 0, nullptr);
        }
        m_running = true;
        PROPVARIANT pv; PropVariantInit(&pv);
        QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &pv);
    }
    void End(bool announce) {
        m_running = false;
        if (m_thread) { SetEvent(m_hStop); WaitForSingleObject(m_thread, 3000); CloseHandle(m_thread); m_thread = nullptr; }
        EnterCriticalSection(&m_cs);
        for (size_t i = 0; i < m_tokens.size(); i++) if (m_tokens[i]) m_tokens[i]->Release();
        m_tokens.clear();
        LeaveCriticalSection(&m_cs);
        if (announce) { PROPVARIANT pv; PropVariantInit(&pv); QueueEvent(MEStreamStopped, GUID_NULL, S_OK, &pv); }
    }
    void Shutdown() {
        m_shut = true;
        End(false);
        if (m_q) { m_q->Shutdown(); m_q->Release(); m_q = nullptr; }
        m_src = nullptr;
    }
    ~UsbCamStream() {
        End(false);
        if (m_q) { m_q->Shutdown(); m_q->Release(); }
        if (m_hStop) CloseHandle(m_hStop);
        if (m_hReq) CloseHandle(m_hReq);
        m_sd->Release();
        DeleteCriticalSection(&m_cs);
        InterlockedDecrement(&g_objects);
    }

private:
    static DWORD WINAPI ThreadMain(LPVOID p) { static_cast<UsbCamStream*>(p)->Run(); return 0; }

    bool PendingRequest() {
        EnterCriticalSection(&m_cs);
        bool p = !m_tokens.empty();
        LeaveCriticalSection(&m_cs);
        return p;
    }

    void Run() {
        int64_t lastDeliver = 0;
        int64_t lastSeq = 0;
        while (WaitForSingleObject(m_hStop, 0) != WAIT_OBJECT_0) {
            m_sh.Open();
            UcvHeader* hd = m_sh.hdr;
            if (hd) {
                hd->src_tick_ms = UcvNowUs() / 1000;          // "an app is using the camera" heartbeat
                hd->want_w = (LONG)m_w; hd->want_h = (LONG)m_h;
            }
            if (!PendingRequest()) {
                HANDLE hs[2] = { m_hStop, m_hReq };
                WaitForMultipleObjects(2, hs, FALSE, 100);
                continue;
            }
            int64_t seq = hd ? hd->seq : 0;
            int64_t now = UcvNowUs();
            if (seq != 0 && seq != lastSeq) { Deliver(seq, false); lastSeq = seq; lastDeliver = UcvNowUs(); continue; }
            if (now - lastDeliver >= 66000) { Deliver(lastSeq, true); lastDeliver = UcvNowUs(); continue; }
            DWORD ms = (DWORD)((66000 - (now - lastDeliver)) / 1000) + 1;
            if (ms > 100) ms = 100;
            if (m_sh.evt) { HANDLE hs[2] = { m_hStop, m_sh.evt }; WaitForMultipleObjects(2, hs, FALSE, ms); }
            else WaitForSingleObject(m_hStop, ms);
        }
        m_sh.Close();
    }

    // seq == 0 -> nothing to show yet: flat colour frame. dup = repeat of an already delivered frame (no stats).
    void Deliver(int64_t seq, bool dup) {
        IUnknown* token = nullptr; bool have = false;
        EnterCriticalSection(&m_cs);
        if (!m_tokens.empty()) { token = m_tokens.front(); m_tokens.erase(m_tokens.begin()); have = true; }
        LeaveCriticalSection(&m_cs);
        if (!have) return;

        const size_t outBytes = FrameBytes(m_fmt, m_w, m_h);
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer((DWORD)outBytes, &buf))) { if (token) token->Release(); return; }
        BYTE* dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) { buf->Release(); if (token) token->Release(); return; }

        UcvHeader* hd = m_sh.hdr;
        const uint8_t* src = nullptr;
        int64_t usedSeq = 0; int64_t written = 0;
        for (int tries = 0; tries < 3 && hd && seq > 0; tries++) {
            UcvSlotInfo si = hd->slot[seq % UCV_SLOTS];
            if (si.w == m_w && si.h == m_h) {
                // copy first (newest wins, nothing is queued), then check the writer did not lap this slot meanwhile
                if (m_stage.size() < (size_t)m_w * m_h * 3 / 2) m_stage.resize((size_t)m_w * m_h * 3 / 2);
                memcpy(m_stage.data(), UcvSlotPtr(m_sh.base, seq), (size_t)m_w * m_h * 3 / 2);
                if (hd->seq - seq < UCV_SLOTS - 1) { src = m_stage.data(); usedSeq = seq; written = si.t_written_us; break; }
                seq = hd->seq;
            } else break;   // size not (yet) what the app asked for: show the flat frame for a moment
        }
        if (!src) {
            size_t n = (size_t)m_w * m_h;
            if (m_flat.size() != n * 3 / 2 || m_flatW != m_w) {
                m_flat.assign(n * 3 / 2, 0);
                memset(m_flat.data(), 48, n);
                for (size_t i = 0; i < n / 2; i += 2) { m_flat[n + i] = 140; m_flat[n + i + 1] = 118; }
                m_flatW = m_w;
            }
            src = m_flat.data();
        }
        ConvertFrame(m_fmt, src, m_w, m_h, dst);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)outBytes);

        IMFSample* smp = nullptr;
        if (SUCCEEDED(MFCreateSample(&smp))) {
            smp->AddBuffer(buf);
            LONGLONG t = MFGetSystemTime();
            smp->SetSampleTime(t);
            smp->SetSampleDuration(333333);
            smp->SetUINT64(MFSampleExtension_DeviceTimestamp, (UINT64)t);
            if (token) smp->SetUnknown(MFSampleExtension_Token, token);
            if (m_q) m_q->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, smp);
            smp->Release();
        }
        buf->Release();
        if (token) token->Release();

        if (hd) {
            hd->src_frames++;
            if (dup || !usedSeq) hd->src_dups++;
            else if (written) {
                int64_t lat = UcvNowUs() - written;
                if (lat < 0) lat = 0;
                InterlockedAdd64(&hd->src_lat_sum_us, lat);
                if (lat > hd->src_lat_max_us) hd->src_lat_max_us = lat;
            }
        }
    }

    LONG m_ref = 1;
    UsbCamSource* m_src;
    IMFStreamDescriptor* m_sd;
    IMFMediaEventQueue* m_q = nullptr;
    CRITICAL_SECTION m_cs;
    std::vector<IUnknown*> m_tokens;
    HANDLE m_hStop = nullptr, m_hReq = nullptr, m_thread = nullptr;
    volatile bool m_running = false, m_shut = false;
    Fmt m_fmt = F_NV12; UINT m_w = 1280, m_h = 720;
    Shared m_sh;
    std::vector<uint8_t> m_stage, m_flat; UINT m_flatW = 0;
};

// ---------------- the source ----------------
class UsbCamSource : public IMFMediaSourceEx, public IMFGetService, public IKsControl {
public:
    UsbCamSource() { InterlockedIncrement(&g_objects); InitializeCriticalSection(&m_cs); MFStartup(MF_VERSION, MFSTARTUP_LITE); }
    HRESULT Init() {
        HRESULT hr = MFCreateEventQueue(&m_q);
        if (FAILED(hr)) return hr;
        std::vector<IMFMediaType*> types;
        for (int i = 0; i < kModeCount; i++) { IMFMediaType* t = nullptr; if (SUCCEEDED(MakeType(kModes[i], &t))) types.push_back(t); }
        if (types.empty()) return E_FAIL;
        hr = MFCreateStreamDescriptor(0, (DWORD)types.size(), types.data(), &m_sd);
        if (SUCCEEDED(hr)) {
            IMFMediaTypeHandler* h = nullptr;
            if (SUCCEEDED(m_sd->GetMediaTypeHandler(&h))) { h->SetCurrentMediaType(types[0]); h->Release(); }
            m_sd->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
            m_sd->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinCapture);
            m_sd->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
            m_sd->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, (UINT32)MFFrameSourceTypes_Color);
        }
        for (size_t i = 0; i < types.size(); i++) types[i]->Release();
        if (FAILED(hr)) return hr;
        hr = MFCreatePresentationDescriptor(1, &m_sd, &m_pd);
        if (FAILED(hr)) return hr;
        m_pd->SelectStream(0);
        hr = MFCreateAttributes(&m_attr, 2);
        if (FAILED(hr)) return hr;
        m_stream = new (std::nothrow) UsbCamStream(this, m_sd);
        return m_stream ? S_OK : E_OUTOFMEMORY;
    }
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) *ppv = static_cast<IMFMediaSourceEx*>(this);
        else if (riid == IID_IMFGetService) *ppv = static_cast<IMFGetService*>(this);
        else if (riid == kIID_IKsControl) *ppv = static_cast<IKsControl*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return (ULONG)r; }
    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override { return m_q ? m_q->GetEvent(f, e) : MF_E_SHUTDOWN; }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return m_q ? m_q->BeginGetEvent(cb, st) : MF_E_SHUTDOWN; }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return m_q ? m_q->EndGetEvent(r, e) : MF_E_SHUTDOWN; }
    STDMETHODIMP QueueEvent(MediaEventType t, REFGUID ext, HRESULT hr, const PROPVARIANT* v) override { return m_q ? m_q->QueueEventParamVar(t, ext, hr, v) : MF_E_SHUTDOWN; }
    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* c) override { if (!c) return E_POINTER; *c = MFMEDIASOURCE_IS_LIVE; return S_OK; }
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pp) override {
        if (!pp) return E_POINTER;
        if (m_shut || !m_pd) return MF_E_SHUTDOWN;
        return m_pd->Clone(pp);
    }
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmtTime, const PROPVARIANT* start) override {
        if (m_shut || !m_stream) return MF_E_SHUTDOWN;
        if (!pd) return E_INVALIDARG;
        if (fmtTime && *fmtTime != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
        Fmt f = F_NV12; UINT w = 1280, h = 720;
        IMFStreamDescriptor* sd = nullptr; BOOL sel = FALSE;
        if (SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &sel, &sd)) && sd) {
            IMFMediaTypeHandler* th = nullptr; IMFMediaType* mt = nullptr;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&th)) && th) {
                if (SUCCEEDED(th->GetCurrentMediaType(&mt)) && mt) {
                    GUID sub = GUID_NULL; mt->GetGUID(MF_MT_SUBTYPE, &sub);
                    f = (sub == MFVideoFormat_YUY2) ? F_YUY2 : (sub == MFVideoFormat_RGB32 || sub == MFVideoFormat_ARGB32) ? F_RGB32 : F_NV12;
                    UINT32 mw = 0, mh = 0;
                    if (SUCCEEDED(MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &mw, &mh)) && mw && mh && mw <= UCV_MAX_W && mh <= UCV_MAX_H && !(mw & 1) && !(mh & 1)) { w = mw; h = mh; }
                    mt->Release();
                }
                th->Release();
            }
            sd->Release();
        }
        PROPVARIANT none; PropVariantInit(&none);
        PROPVARIANT unk; PropVariantInit(&unk);
        unk.vt = VT_UNKNOWN; unk.punkVal = static_cast<IMFMediaStream2*>(m_stream);
        m_stream->AddRef();   // PropVariant owns one reference
        QueueEvent(m_started ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, &unk);
        PropVariantClear(&unk);
        m_stream->Begin(f, w, h);
        m_started = true;
        QueueEvent(MESourceStarted, GUID_NULL, S_OK, start ? start : &none);
        return S_OK;
    }
    STDMETHODIMP Stop() override {
        if (m_shut) return MF_E_SHUTDOWN;
        if (m_stream) m_stream->End(true);
        PROPVARIANT none; PropVariantInit(&none);
        QueueEvent(MESourceStopped, GUID_NULL, S_OK, &none);
        return S_OK;
    }
    STDMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
    STDMETHODIMP Shutdown() override {
        if (m_shut) return S_OK;
        m_shut = true;
        if (m_stream) m_stream->Shutdown();
        if (m_q) { m_q->Shutdown(); }
        return S_OK;
    }
    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** pp) override {
        if (!pp) return E_POINTER; if (!m_attr) return MF_E_SHUTDOWN;
        *pp = m_attr; m_attr->AddRef(); return S_OK;
    }
    STDMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** pp) override {
        if (!pp) return E_POINTER; if (!m_sd) return MF_E_SHUTDOWN;
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        *pp = m_sd; m_sd->AddRef(); return S_OK;
    }
    STDMETHODIMP SetD3DManager(IUnknown*) override { return S_OK; }
    // IMFGetService
    STDMETHODIMP GetService(REFGUID, REFIID, LPVOID* ppv) override { if (ppv) *ppv = nullptr; return MF_E_UNSUPPORTED_SERVICE; }
    // IKsControl: no camera controls (focus, exposure ...) are offered
    STDMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
    STDMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

    ~UsbCamSource() {
        Shutdown();
        if (m_stream) { m_stream->Release(); m_stream = nullptr; }
        if (m_q) { m_q->Release(); m_q = nullptr; }
        if (m_pd) { m_pd->Release(); m_pd = nullptr; }
        if (m_sd) { m_sd->Release(); m_sd = nullptr; }
        if (m_attr) { m_attr->Release(); m_attr = nullptr; }
        DeleteCriticalSection(&m_cs);
        MFShutdown();
        InterlockedDecrement(&g_objects);
    }

private:
    LONG m_ref = 1;
    CRITICAL_SECTION m_cs;
    IMFMediaEventQueue* m_q = nullptr;
    IMFStreamDescriptor* m_sd = nullptr;
    IMFPresentationDescriptor* m_pd = nullptr;
    IMFAttributes* m_attr = nullptr;
    UsbCamStream* m_stream = nullptr;
    volatile bool m_shut = false;
    bool m_started = false;
};

STDMETHODIMP UsbCamStream::GetMediaSource(IMFMediaSource** pp) {
    if (!pp) return E_POINTER;
    if (!m_src) return MF_E_SHUTDOWN;
    return m_src->QueryInterface(IID_IMFMediaSource, (void**)pp);
}

// ---------------- IMFActivate wrapper: Frame Server creates the registered class and asks for IMFActivate ----------------
class Activator : public IMFActivate {
public:
    Activator() { InterlockedIncrement(&g_objects); MFCreateAttributes(&m_a, 0); }
    ~Activator() { if (m_a) m_a->Release(); InterlockedDecrement(&g_objects); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFActivate || riid == IID_IMFAttributes) { *ppv = static_cast<IMFActivate*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return (ULONG)r; }
    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        UsbCamSource* s = new (std::nothrow) UsbCamSource();
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = s->Init();
        if (SUCCEEDED(hr)) hr = s->QueryInterface(riid, ppv);
        s->Release();
        return hr;
    }
    STDMETHODIMP ShutdownObject() override { return S_OK; }
    STDMETHODIMP DetachObject() override { return S_OK; }
    // IMFAttributes: plain delegation to a standard attribute store
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return m_a->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return m_a->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return m_a->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return m_a->Compare(o, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return m_a->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return m_a->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return m_a->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return m_a->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* n) override { return m_a->GetStringLength(k, n); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR b, UINT32 n, UINT32* l) override { return m_a->GetString(k, b, n, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* b, UINT32* l) override { return m_a->GetAllocatedString(k, b, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* n) override { return m_a->GetBlobSize(k, n); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* l) override { return m_a->GetBlob(k, b, n, l); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* n) override { return m_a->GetAllocatedBlob(k, b, n); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID r, LPVOID* v) override { return m_a->GetUnknown(k, r, v); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return m_a->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return m_a->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return m_a->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return m_a->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return m_a->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return m_a->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return m_a->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return m_a->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return m_a->SetBlob(k, b, n); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return m_a->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return m_a->LockStore(); }
    STDMETHODIMP UnlockStore() override { return m_a->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* n) override { return m_a->GetCount(n); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return m_a->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) override { return m_a->CopyAllItems(d); }
private:
    LONG m_ref = 1;
    IMFAttributes* m_a = nullptr;
};

// ---------------- COM plumbing ----------------
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (r == 0) delete this; return (ULONG)r; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (riid == IID_IMFActivate || riid == IID_IMFAttributes) {
            Activator* a = new (std::nothrow) Activator();
            if (!a) return E_OUTOFMEMORY;
            HRESULT hr = a->QueryInterface(riid, ppv);
            a->Release();
            return hr;
        }
        UsbCamSource* s = new (std::nothrow) UsbCamSource();
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = s->Init();
        if (SUCCEEDED(hr)) hr = s->QueryInterface(riid, ppv);
        s->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL l) override { if (l) InterlockedIncrement(&g_objects); else InterlockedDecrement(&g_objects); return S_OK; }
private:
    LONG m_ref = 1;
};

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_UsbCamSource) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_mod, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const wchar_t* key = L"Software\\Classes\\CLSID\\" UCV_CLSID_STR;
    HKEY k = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    const wchar_t* nm = L"UsbCam Camera media source";
    RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)nm, (DWORD)((wcslen(nm) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    wchar_t sub[256]; wsprintfW(sub, L"%s\\InprocServer32", key);
    r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
    const wchar_t* tm = L"Both";
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)tm, (DWORD)((wcslen(tm) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer() {
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" UCV_CLSID_STR L"\\InprocServer32", KEY_WOW64_64KEY, 0);
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" UCV_CLSID_STR, KEY_WOW64_64KEY, 0);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_mod = (HMODULE)h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
