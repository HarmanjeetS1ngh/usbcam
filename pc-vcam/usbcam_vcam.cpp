// usbcam_vcam: feeder for the "UsbCam Camera" Windows virtual camera.
// Creates the virtual camera (Media Foundation, Windows 11), and while an app is using it: adb forward + control channel ->
// framed H.264 -> D3D11VA decode -> crop/scale (+mirror) -> newest frame into shared memory read by usbcam_vcam_src.dll.
// Phone unplugged / camera off / no stream: writes a "Waiting for phone" picture instead, so apps never see a dead camera.
// Control (tray): 127.0.0.1:5003, one JSON line in, one JSON line out: status | mirror {on} | quit.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <objbase.h>
#include <pdh.h>
#include <pdhmsg.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "vcam_shared.h"

using Clock = std::chrono::steady_clock;
static int64_t now_us() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static int64_t qpc_ms() { return UcvNowUs() / 1000; }

struct Cfg { std::string cam; int w = 0, h = 0, fps = 0, br = 0, ll = -1; bool sw = false; int seconds = 0; int idle_s = 15; int mirror = 0; int port = 5003; } g_cfg;

static std::atomic<bool> g_run{true};
static std::atomic<int> g_mirror{0};
static std::atomic<bool> g_vcam_on{false};
static std::atomic<bool> g_gui_active{false};   // GUI window visible => keep the phone streaming
static std::atomic<bool> g_restart{false};      // GUI changed the picture settings => restart the phone session
static std::mutex g_cfgmu;                      // guards g_cfg (the GUI changes it while a session runs)
static std::mutex g_infomu;                     // guards the three strings below
static std::string g_caps, g_pstatus, g_actual; // phone `caps` reply, last `status` reply, what the phone is really sending
static std::mutex g_prevmu;                     // preview frame (newest wins, only kept while the GUI is open)
static AVFrame* g_prev = nullptr;
static bool g_prev_new = false;
static std::atomic<int64_t> g_prev_us{0};
static bool g_vcam_supported = false;
static std::string g_vcam_why;
static void preview_push(AVFrame* f) {
    std::lock_guard<std::mutex> l(g_prevmu);
    if (!g_prev) return;
    av_frame_unref(g_prev);
    if (av_frame_ref(g_prev, f) < 0) return;
    g_prev_new = true;
    g_prev_us = now_us();
}

// ---------- log (short, to %TEMP%\usbcam-vcam.log, also to the console) ----------
static FILE* g_log = nullptr;
static std::mutex g_logmu;
static void ulog(const char* fmt, ...) {
    char msg[600];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[700];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    std::lock_guard<std::mutex> l(g_logmu);
    fputs(line, stdout);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
}
static void log_open() {
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (!n || n >= MAX_PATH) return;
    wsprintfW(path, L"%susbcam-vcam.log", tmp);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    const wchar_t* mode = L"a";
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 1000000) mode = L"w";
    g_log = _wfopen(path, mode);
}

// ---------- shared memory (opened here, created by the DLL) ----------
struct Sh { HANDLE map = nullptr, evt = nullptr; UcvHeader* hdr = nullptr; uint8_t* base = nullptr; } g_sh;
static std::mutex g_wmu;  // one writer at a time (live frames / placeholder)

static void sh_open_thread() {
    const wchar_t* names[2][2] = { { UCV_MAP_GLOBAL, UCV_EVT_GLOBAL }, { UCV_MAP_LOCAL, UCV_EVT_LOCAL } };
    while (g_run && !g_sh.hdr) {
        for (int i = 0; i < 2 && !g_sh.hdr; i++) {
            HANDLE m = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, names[i][0]);
            if (!m) continue;
            void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (!v) { CloseHandle(m); continue; }
            HANDLE e = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, names[i][1]);
            UcvHeader* h = (UcvHeader*)v;
            if (h->magic != UCV_MAGIC || h->version != UCV_VERSION || !e) { UnmapViewOfFile(v); CloseHandle(m); if (e) CloseHandle(e); continue; }
            g_sh.map = m; g_sh.evt = e; g_sh.base = (uint8_t*)v;
            h->feeder_pid = (LONG)GetCurrentProcessId();
            h->mirror = g_mirror.load();
            g_sh.hdr = h;
            ulog("[shm] connected to the camera component (%ls)", names[i][0]);
        }
        if (!g_sh.hdr) sleep_ms(500);
    }
}

static bool consumer_recent(int64_t within_ms) {
    UcvHeader* h = g_sh.hdr;
    if (!h) return false;
    int64_t t = h->src_tick_ms;
    return t != 0 && (qpc_ms() - t) < within_ms;
}

// the phone must stream while the GUI window is visible or an app has the virtual camera open
static bool need_stream() { return g_gui_active.load() || consumer_recent(2000); }

// ---------- status text (for the placeholder and the tray) ----------
static std::mutex g_smu;
static std::string g_status = "starting", g_sub = "Connecting to the phone";
static std::atomic<int64_t> g_last_live_us{0};
static void set_status(const std::string& s, const std::string& sub) {
    std::lock_guard<std::mutex> l(g_smu);
    if (s != g_status) { g_status = s; ulog("[status] %s", s.c_str()); }
    g_sub = sub;
}
static std::string get_sub() { std::lock_guard<std::mutex> l(g_smu); return g_sub; }
static std::string get_status() { std::lock_guard<std::mutex> l(g_smu); return g_status; }

// ---------- crop (fill) + scale + mirror, NV12 -> NV12 ----------
template <int CH>
static void ScalePlane(const uint8_t* s, int ss, int sw, int sh, uint8_t* d, int ds, int dw, int dh, bool mir) {
    std::vector<int> x0(dw), x1(dw), xf(dw);
    for (int x = 0; x < dw; x++) {
        int xx = mir ? dw - 1 - x : x;
        float fx = ((float)xx + 0.5f) * (float)sw / (float)dw - 0.5f; if (fx < 0) fx = 0;
        int i = (int)fx, f = (int)((fx - (float)i) * 256.0f);
        if (i >= sw - 1) { i = sw - 1; f = 0; }
        x0[x] = i * CH; x1[x] = (i + 1 < sw ? i + 1 : i) * CH; xf[x] = f;
    }
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f; if (fy < 0) fy = 0;
        int j = (int)fy, wy = (int)((fy - (float)j) * 256.0f);
        if (j >= sh - 1) { j = sh - 1; wy = 0; }
        const uint8_t* r0 = s + (size_t)j * ss;
        const uint8_t* r1 = s + (size_t)(j + 1 < sh ? j + 1 : j) * ss;
        uint8_t* o = d + (size_t)y * ds;
        for (int x = 0; x < dw; x++) {
            int a = x0[x], b = x1[x], f = xf[x];
            for (int c = 0; c < CH; c++) {
                int top = r0[a + c] * (256 - f) + r0[b + c] * f;
                int bot = r1[a + c] * (256 - f) + r1[b + c] * f;
                o[x * CH + c] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
            }
        }
    }
}

static void CopyRows(const uint8_t* s, int ss, uint8_t* d, int ds, int bytes, int rows, bool mir, int unit) {
    for (int y = 0; y < rows; y++) {
        const uint8_t* r = s + (size_t)y * ss; uint8_t* o = d + (size_t)y * ds;
        if (!mir) { memcpy(o, r, (size_t)bytes); continue; }
        int n = bytes / unit;
        for (int i = 0; i < n; i++) for (int k = 0; k < unit; k++) o[i * unit + k] = r[(n - 1 - i) * unit + k];
    }
}

// crop the source to the target's aspect ratio (centre), then scale to dw x dh; sw/sh must be even
static void ScaleNV12(const uint8_t* Y, int ys, const uint8_t* UV, int uvs, int sw, int sh, uint8_t* dY, uint8_t* dUV, int dw, int dh, bool mir) {
    int cw = sw, ch = sh, cx = 0, cy = 0;
    if ((int64_t)sw * dh > (int64_t)sh * dw) { cw = (int)((int64_t)sh * dw / dh) & ~1; cx = ((sw - cw) / 2) & ~1; }
    else { ch = (int)((int64_t)sw * dh / dw) & ~1; cy = ((sh - ch) / 2) & ~1; }
    const uint8_t* cY = Y + (size_t)cy * ys + cx;
    const uint8_t* cUV = UV + (size_t)(cy / 2) * uvs + cx;
    if (cw == dw && ch == dh) {
        CopyRows(cY, ys, dY, dw, dw, dh, mir, 1);
        CopyRows(cUV, uvs, dUV, dw, dw, dh / 2, mir, 2);
    } else {
        ScalePlane<1>(cY, ys, cw, ch, dY, dw, dw, dh, mir);
        ScalePlane<2>(cUV, uvs, cw / 2, ch / 2, dUV, dw, dw / 2, dh / 2, mir);
    }
}

// publish one frame (caller holds g_wmu): newest wins, 4 slots round robin, nothing is queued
static void PublishSlot(const uint8_t* Y, int ys, const uint8_t* UV, int uvs, int sw, int sh, int dw, int dh, bool mir, bool live) {
    UcvHeader* hd = g_sh.hdr;
    int64_t next = hd->seq + 1;
    uint8_t* base = UcvSlotPtr(g_sh.base, next);
    ScaleNV12(Y, ys, UV, uvs, sw, sh, base, base + (size_t)dw * dh, dw, dh, mir);
    UcvSlotInfo& si = hd->slot[next % UCV_SLOTS];
    si.w = (uint32_t)dw; si.h = (uint32_t)dh; si.live = live ? 1u : 0u; si.t_written_us = UcvNowUs();
    InterlockedExchange64(&hd->seq, next);
    SetEvent(g_sh.evt);
}

static bool want_size(int& w, int& h) {
    UcvHeader* hd = g_sh.hdr;
    if (!hd) return false;
    w = hd->want_w; h = hd->want_h;
    return w > 0 && h > 0 && w <= UCV_MAX_W && h <= UCV_MAX_H && !(w & 1) && !(h & 1);
}

static std::atomic<int64_t> g_wr_us{0}, g_dec_us{0};
static std::atomic<int> g_frames{0};
static std::vector<uint8_t> g_tmpuv;

static void write_live(AVFrame* f) {
    int tw, th;
    if (!want_size(tw, th)) return;
    const uint8_t* Y = f->data[0]; int ys = f->linesize[0];
    const uint8_t* UV = nullptr; int uvs = 0;
    if (f->format == AV_PIX_FMT_NV12) { UV = f->data[1]; uvs = f->linesize[1]; }
    else if (f->format == AV_PIX_FMT_YUV420P) {
        int cw = f->width / 2, ch = f->height / 2;
        g_tmpuv.resize((size_t)cw * 2 * ch);
        for (int y = 0; y < ch; y++) {
            const uint8_t* u = f->data[1] + (size_t)y * f->linesize[1];
            const uint8_t* v = f->data[2] + (size_t)y * f->linesize[2];
            uint8_t* o = g_tmpuv.data() + (size_t)y * cw * 2;
            for (int x = 0; x < cw; x++) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
        }
        UV = g_tmpuv.data(); uvs = cw * 2;
    } else { static bool w = false; if (!w) { ulog("[warn] unsupported decoder pixel format %d", f->format); w = true; } return; }
    if (f->width < 2 || f->height < 2 || (f->width & 1) || (f->height & 1)) return;
    std::lock_guard<std::mutex> l(g_wmu);
    PublishSlot(Y, ys, UV, uvs, f->width, f->height, tw, th, g_mirror.load() != 0, true);
    g_last_live_us = now_us();
}

// ---------- "Waiting for phone" picture (GDI, BT.709 limited NV12) ----------
static void RenderPlaceholder(int W, int H, int dots, const std::string& sub, std::vector<uint8_t>& nv12) {
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    nv12.assign((size_t)W * H * 3 / 2, 0);
    if (!dc || !bmp || !bits) { memset(nv12.data(), 40, (size_t)W * H); if (dc) DeleteDC(dc); if (bmp) DeleteObject(bmp); return; }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all = { 0, 0, W, H };
    HBRUSH bg = CreateSolidBrush(RGB(22, 27, 46));
    FillRect(dc, &all, bg); DeleteObject(bg);
    // little phone outline
    int pw = H / 9, ph = H / 4, cx = W / 2, top = H / 2 - ph - H / 14;
    HPEN pen = CreatePen(PS_SOLID, H / 120 + 1, RGB(200, 208, 230));
    HGDIOBJ oldPen = SelectObject(dc, pen); HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - pw / 2, top, cx + pw / 2, top + ph, pw / 4, pw / 4);
    RoundRect(dc, cx - pw / 8, top + ph - ph / 8, cx + pw / 8, top + ph - ph / 14, 2, 2);
    SelectObject(dc, oldPen); SelectObject(dc, oldBr); DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    HFONT fontBig = CreateFontW(-H / 11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT fontSmall = CreateFontW(-H / 24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, fontBig);
    const wchar_t* title = L"Waiting for phone";
    SIZE ts = {}; GetTextExtentPoint32W(dc, title, (int)wcslen(title), &ts);
    int tx = (W - ts.cx) / 2, ty = H / 2 + H / 40;
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, tx, ty, title, (int)wcslen(title));
    wchar_t dd[4] = L"   "; for (int i = 0; i < dots && i < 3; i++) dd[i] = L'.';
    TextOutW(dc, tx + ts.cx, ty, dd, 3);
    SelectObject(dc, fontSmall);
    wchar_t wsub[200] = {}; MultiByteToWideChar(CP_UTF8, 0, sub.c_str(), -1, wsub, 199);
    SIZE ss = {}; GetTextExtentPoint32W(dc, wsub, (int)wcslen(wsub), &ss);
    SetTextColor(dc, RGB(150, 160, 190));
    TextOutW(dc, (W - ss.cx) / 2, ty + ts.cy + H / 40, wsub, (int)wcslen(wsub));
    GdiFlush();
    SelectObject(dc, oldFont); DeleteObject(fontBig); DeleteObject(fontSmall);
    const uint8_t* px = (const uint8_t*)bits;   // BGRA
    uint8_t* Y = nv12.data(); uint8_t* UV = Y + (size_t)W * H;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const uint8_t* p = px + ((size_t)y * W + x) * 4;
        Y[(size_t)y * W + x] = (uint8_t)(16 + ((47 * p[2] + 157 * p[1] + 16 * p[0] + 128) >> 8));
    }
    for (int y = 0; y < H / 2; y++) for (int x = 0; x < W / 2; x++) {
        int r = 0, g = 0, b = 0;
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) { const uint8_t* p = px + ((size_t)(2 * y + dy) * W + 2 * x + dx) * 4; r += p[2]; g += p[1]; b += p[0]; }
        r /= 4; g /= 4; b /= 4;
        UV[(size_t)y * W + 2 * x] = (uint8_t)(128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8));
        UV[(size_t)y * W + 2 * x + 1] = (uint8_t)(128 + ((112 * r - 102 * g - 10 * b + 128) >> 8));
    }
    SelectObject(dc, oldBmp); DeleteObject(bmp); DeleteDC(dc);
}

static void placeholder_thread() {
    std::vector<uint8_t> frame; int cw = 0, ch = 0, cdots = -1; std::string csub;
    while (g_run) {
        sleep_ms(120);
        int tw, th;
        if (!want_size(tw, th) || !consumer_recent(2500)) { cw = 0; continue; }
        int64_t now = now_us();
        if (now - g_last_live_us.load() < 800000) { cw = 0; continue; }   // live frames are flowing
        int dots = (int)((now / 500000) % 4);
        std::string sub = get_sub();
        if (tw == cw && th == ch && dots == cdots && sub == csub) continue;
        RenderPlaceholder(tw, th, dots, sub, frame);
        cw = tw; ch = th; cdots = dots; csub = sub;
        std::lock_guard<std::mutex> l(g_wmu);
        if (now_us() - g_last_live_us.load() < 800000) continue;
        PublishSlot(frame.data(), tw, frame.data() + (size_t)tw * th, tw, tw, th, tw, th, false, false);
    }
}

// ---------- process / network helpers (same as the viewer) ----------
static std::string run_out(const std::string& cmd) {
    // runs `cmd.exe /c <cmd>` without a console window (this also runs inside the windowed app) and returns its output
    std::string out;
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nul;
    std::wstring w = L"cmd.exe /d /c ";
    w.append(cmd.begin(), cmd.end());
    std::vector<wchar_t> buf(w.begin(), w.end()); buf.push_back(0);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (ok) {
        char b[512]; DWORD n = 0;
        while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out.append(b, n);
        if (WaitForSingleObject(pi.hProcess, 8000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}
static SOCKET tcp_connect(int port, int rcv_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in a = {}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    DWORD t = (DWORD)rcv_ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    return s;
}
static bool read_n(SOCKET s, uint8_t* b, size_t n) {
    size_t o = 0;
    while (o < n) { int k = recv(s, (char*)b + o, (int)(n - o), 0); if (k <= 0) return false; o += (size_t)k; }
    return true;
}
static bool ctl_cmd(SOCKET s, const std::string& json, std::string& reply) {
    std::string m = json + "\n";
    if (send(s, m.data(), (int)m.size(), 0) <= 0) return false;
    reply.clear();
    char c;
    while (true) { int k = recv(s, &c, 1, 0); if (k <= 0) return false; if (c == '\n') return true; reply += c; }
}
static bool adb_ready() {
    if (run_out("adb get-state 2>&1") != "device") return false;
    run_out("adb forward tcp:5000 tcp:5000 2>&1");
    run_out("adb forward tcp:5002 tcp:5002 2>&1");
    return true;
}
static int64_t json_int(const std::string& s, const char* key) {
    size_t p = s.find(key);
    return p == std::string::npos ? 0 : atoll(s.c_str() + p + strlen(key));
}

// ---------- decoder (same settings as the viewer) ----------
struct Dec { AVCodecContext* c = nullptr; AVBufferRef* hw = nullptr; AVFrame *f = nullptr, *sw = nullptr; AVPacket* p = nullptr; };
static std::string g_hw = "?";
static enum AVPixelFormat get_fmt(AVCodecContext* c, const enum AVPixelFormat* f) {
    for (const enum AVPixelFormat* q = f; *q != AV_PIX_FMT_NONE; q++) if (*q == AV_PIX_FMT_D3D11) return *q;
    return avcodec_default_get_format(c, f);
}
static bool dec_open(Dec& d) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    d.c = avcodec_alloc_context3(codec);
    d.c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d.c->flags2 |= AV_CODEC_FLAG2_FAST;
    d.c->thread_count = 1;
    g_hw = "software";
    if (!g_cfg.sw && av_hwdevice_ctx_create(&d.hw, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
        d.c->hw_device_ctx = av_buffer_ref(d.hw);
        d.c->get_format = get_fmt;
        g_hw = "d3d11va";
    }
    if (avcodec_open2(d.c, codec, nullptr) < 0) return false;
    d.f = av_frame_alloc(); d.sw = av_frame_alloc(); d.p = av_packet_alloc();
    return true;
}
static void dec_close(Dec& d) {
    av_frame_free(&d.f); av_frame_free(&d.sw); av_packet_free(&d.p);
    avcodec_free_context(&d.c); av_buffer_unref(&d.hw);
    d = Dec();
}

// ---------- one phone session ----------
static std::atomic<int64_t> g_off{0}, g_off2{0};
static std::atomic<bool> g_off_ok{false}, g_off2_ok{false};
static std::atomic<int> g_dom{0};
static std::atomic<int> g_lat_n{0};
static std::atomic<int64_t> g_lat_sum_us{0};
static std::atomic<SOCKET> g_vid{INVALID_SOCKET};

static void session() {
    SOCKET ctl = INVALID_SOCKET; std::string r;
    for (int i = 0; i < 8 && g_run && need_stream(); i++) {
        ctl = tcp_connect(5002, 3000);
        if (ctl != INVALID_SOCKET) {
            if (ctl_cmd(ctl, "{\"cmd\":\"hello\"}", r) && r.find("\"ok\":true") != std::string::npos) break;
            closesocket(ctl); ctl = INVALID_SOCKET;
        }
        if (i == 0) { set_status("starting UsbCam on the phone", "Opening the UsbCam app on the phone (keep it unlocked)"); run_out("adb shell am start -n com.harman.usbcam/.MainActivity 2>&1"); }
        sleep_ms(1200);
    }
    if (ctl == INVALID_SOCKET) { set_status("cannot reach the UsbCam app", "Open the UsbCam app on the phone"); return; }
    if (g_gui_active) { std::string cr; if (ctl_cmd(ctl, "{\"cmd\":\"caps\"}", cr)) { std::lock_guard<std::mutex> l(g_infomu); g_caps = cr; } }

    int64_t best = INT64_MAX, off = 0, off2 = 0; bool has2 = false;
    for (int i = 0; i < 6; i++) {
        int64_t t0 = now_us();
        if (!ctl_cmd(ctl, "{\"cmd\":\"time\"}", r)) break;
        int64_t t1 = now_us(), p = json_int(r, "\"t_us\":"), pm = json_int(r, "\"mono_us\":");
        if (p && t1 - t0 < best) { best = t1 - t0; off = (t0 + t1) / 2 - p; if (pm) { off2 = (t0 + t1) / 2 - pm; has2 = true; } }
    }
    g_off = off; g_off_ok = best != INT64_MAX; g_off2 = off2; g_off2_ok = has2; g_dom = 0;

    g_restart = false;
    Cfg cf; { std::lock_guard<std::mutex> l(g_cfgmu); cf = g_cfg; }
    std::string st = "{\"cmd\":\"start\",\"hdr\":true";
    if (!cf.cam.empty()) st += ",\"cam\":\"" + cf.cam + "\"";
    if (cf.w) st += ",\"w\":" + std::to_string(cf.w);
    if (cf.h) st += ",\"h\":" + std::to_string(cf.h);
    if (cf.fps) st += ",\"fps\":" + std::to_string(cf.fps);
    if (cf.br) st += ",\"br\":" + std::to_string(cf.br);
    if (cf.ll >= 0) st += ",\"ll\":" + std::to_string(cf.ll);
    st += "}";
    if (!ctl_cmd(ctl, st, r) || r.find("\"ok\":true") == std::string::npos) {
        ulog("[phone] start refused: %s", r.c_str());
        if (r.find("switched off") != std::string::npos) set_status("phone camera is switched off", "The camera is switched off in the UsbCam app");
        else set_status("phone refused start", "The phone did not start the camera");
        closesocket(ctl); sleep_ms(1500); return;
    }
    size_t a = r.find("\"actual\":");
    if (a != std::string::npos) ulog("[phone] %s", r.c_str() + a);
    { std::lock_guard<std::mutex> l(g_infomu); g_actual = a != std::string::npos ? r.substr(a + 9) : std::string(); }

    SOCKET vid = INVALID_SOCKET;
    for (int i = 0; i < 6 && g_run && vid == INVALID_SOCKET; i++) { vid = tcp_connect(5000, 4000); if (vid == INVALID_SOCKET) sleep_ms(400); }
    if (vid == INVALID_SOCKET) { set_status("cannot open video port", "Could not open the video connection"); closesocket(ctl); return; }
    g_vid = vid;
    Dec d;
    if (!dec_open(d)) { set_status("decoder open failed", "Video decoder could not start"); closesocket(vid); closesocket(ctl); return; }
    set_status(std::string("streaming (") + g_hw + ")", "Connecting to the phone");

    std::vector<uint8_t> buf(1 << 20);
    uint8_t h[13];
    bool idle_stop = false, restart = false;
    int64_t t_pst = 0;
    while (g_run) {
        if (g_restart.exchange(false)) { restart = true; break; }                          // the GUI changed the picture settings
        if (!g_gui_active && !consumer_recent((int64_t)g_cfg.idle_s * 1000)) { idle_stop = true; break; }   // no app has used the camera for a while
        if (g_gui_active && now_us() - t_pst > 4000000) {                                  // battery / temperature for the window
            t_pst = now_us();
            std::string sr;
            if (ctl_cmd(ctl, "{\"cmd\":\"status\"}", sr)) { std::lock_guard<std::mutex> l(g_infomu); g_pstatus = sr; }
        }
        if (!read_n(vid, h, 13)) break;
        uint32_t len = ((uint32_t)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        int64_t pts = 0; for (int i = 4; i < 12; i++) pts = (pts << 8) | h[i];
        if (len == 0 || len > 16u * 1024 * 1024) { ulog("[error] bad frame length %u", len); break; }
        if (buf.size() < len + AV_INPUT_BUFFER_PADDING_SIZE) buf.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!read_n(vid, buf.data(), len)) break;
        memset(buf.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        int64_t t0 = now_us();
        d.p->data = buf.data(); d.p->size = (int)len; d.p->pts = pts; d.p->dts = pts;
        if (avcodec_send_packet(d.c, d.p) < 0) continue;
        if (h[12] & 2) continue;
        while (avcodec_receive_frame(d.c, d.f) == 0) {
            AVFrame* out = d.f;
            if (d.f->format == AV_PIX_FMT_D3D11) {
                av_frame_unref(d.sw);
                if (av_hwframe_transfer_data(d.sw, d.f, 0) < 0) { ulog("[warn] hw->sw transfer failed"); continue; }
                out = d.sw;
            }
            int64_t t1 = now_us();
            write_live(out);
            int64_t t2 = now_us();
            g_dec_us += t1 - t0; g_wr_us += t2 - t1; g_frames++;
            if (g_off_ok) {   // camera timestamp -> frame written for the apps (decode + scale included)
                double lb = (t2 - (d.f->pts + g_off.load())) / 1000.0;
                double lm = g_off2_ok ? (t2 - (d.f->pts + g_off2.load())) / 1000.0 : -1e9;
                if (g_dom == 0) { if (lm > -50 && lm < 3000) g_dom = 2; else if (lb > -50 && lb < 3000) g_dom = 1; else g_dom = 3; }
                if (g_dom == 1 || g_dom == 2) { g_lat_sum_us += (int64_t)((g_dom == 2 ? lm : lb) * 1000.0); g_lat_n++; }
            }
            if (g_gui_active) preview_push(out);
            t0 = t2;
        }
    }
    dec_close(d);
    std::string dummy;
    if (!g_run || idle_stop || restart) ctl_cmd(ctl, "{\"cmd\":\"stop\"}", dummy);   // free the phone camera when nobody needs it
    closesocket(vid); g_vid = INVALID_SOCKET; closesocket(ctl);
    if (restart) { ulog("[gui] restarting the phone stream with the new settings"); g_last_live_us = 0; set_status("restarting", "Applying the new picture settings"); }
    else if (idle_stop) { ulog("[idle] no app is using the camera: phone stream stopped"); set_status("idle (no app is using the camera)", "Waiting for an app to open the camera"); g_last_live_us = 0; }
    else if (g_run) set_status("connection lost, retrying", "Connection to the phone lost, reconnecting");
}

static void net_main() {
    while (g_run) {
        if (!need_stream()) {
            if (get_status().rfind("idle", 0) != 0 && get_status().rfind("starting", 0) != 0) set_status("idle (no app is using the camera)", "Waiting for an app to open the camera");
            sleep_ms(150); continue;
        }
        set_status("waiting for phone (adb)", "Plug in the phone with a USB cable (USB debugging on)");
        if (!adb_ready()) { sleep_ms(1500); continue; }
        session();
        if (g_run) sleep_ms(800);
    }
}

// ---------- CPU / GPU numbers for the log ----------
struct CpuTrack { ULONGLONG k = 0, u = 0; int64_t wall = 0; };
static double cpu_pct(HANDLE h, CpuTrack& t, int ncpu) {
    FILETIME c, e, k, u;
    if (!h || !GetProcessTimes(h, &c, &e, &k, &u)) return -1;
    ULONGLONG kk = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime, uu = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
    int64_t w = now_us(); double r = -1;
    if (t.wall && w > t.wall) r = (double)((kk - t.k) + (uu - t.u)) / 10.0 / (double)(w - t.wall) * 100.0 / ncpu;
    t.k = kk; t.u = uu; t.wall = w;
    return r;
}
// busiest GPU engine used by this process (Windows "GPU Engine" counters); -1 = not available
static double gpu_pct(std::string& engine) {
    PDH_HQUERY q = nullptr; double best = -1;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return -1;
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c) == ERROR_SUCCESS) {
        PdhCollectQueryData(q); Sleep(500); PdhCollectQueryData(q);
        DWORD bytes = 0, cnt = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &cnt, nullptr);
        if (st == (PDH_STATUS)PDH_MORE_DATA && bytes) {
            std::vector<BYTE> buf(bytes);
            PDH_FMT_COUNTERVALUE_ITEM_W* it = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &cnt, it) == ERROR_SUCCESS) {
                wchar_t pre[40]; wsprintfW(pre, L"pid_%lu_", GetCurrentProcessId());
                size_t pl = wcslen(pre); best = 0;
                for (DWORD i = 0; i < cnt; i++) {
                    if (!it[i].szName || wcsncmp(it[i].szName, pre, pl) != 0) continue;
                    if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && it[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                    if (it[i].FmtValue.doubleValue > best) {
                        best = it[i].FmtValue.doubleValue;
                        const wchar_t* et = wcsstr(it[i].szName, L"engtype_");
                        char nb[64] = {}; if (et) WideCharToMultiByte(CP_UTF8, 0, et + 8, -1, nb, 63, nullptr, nullptr);
                        engine = nb;
                    }
                }
            }
        }
    }
    PdhCloseQuery(q);
    return best;
}

static std::atomic<double> g_stat_lat{0}, g_stat_fps{0};

static void stats_thread() {
    SYSTEM_INFO si; GetSystemInfo(&si); int ncpu = si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1;
    CpuTrack me, fs; HANDLE self = GetCurrentProcess(); HANDLE fsp = nullptr; LONG fsPid = 0;
    int frames0 = 0; int64_t dec0 = 0, wr0 = 0, latSum0 = 0; int latN0 = 0; int64_t t_last = now_us();
    int64_t sf0 = 0, sd0 = 0, ss0 = 0; int cycle = 0; double lastG = -1; std::string lastEng;
    while (g_run) {
        for (int i = 0; i < 50 && g_run; i++) sleep_ms(100);
        if (!g_run) break;
        UcvHeader* hd = g_sh.hdr;
        int64_t now = now_us(); double s = (now - t_last) / 1e6; t_last = now;
        int fr = g_frames - frames0; int64_t dec = g_dec_us - dec0, wr = g_wr_us - wr0;
        int64_t ls = g_lat_sum_us - latSum0; int ln = g_lat_n - latN0;
        frames0 = g_frames; dec0 = g_dec_us; wr0 = g_wr_us; latSum0 = g_lat_sum_us; latN0 = g_lat_n;
        g_stat_fps = fr / s; g_stat_lat = ln ? ls / 1000.0 / ln : 0.0;
        double c = cpu_pct(self, me, ncpu);
        if (!consumer_recent(6000)) continue;   // keep the log short: numbers only while an app is using the camera
        double fcpu = -1;
        if (hd && hd->src_pid && hd->src_pid != fsPid) { if (fsp) CloseHandle(fsp); fsp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)hd->src_pid); fsPid = hd->src_pid; fs = CpuTrack(); }
        if (fsp) fcpu = cpu_pct(fsp, fs, ncpu);
        int64_t sf = hd ? hd->src_frames : 0, sd = hd ? hd->src_dups : 0, ss = hd ? hd->src_lat_sum_us : 0;
        int64_t real = (sf - sf0) - (sd - sd0);
        double hand = real > 0 ? (ss - ss0) / 1000.0 / real : 0.0;
        double hmax = hd ? hd->src_lat_max_us / 1000.0 : 0.0; if (hd) hd->src_lat_max_us = 0;
        double sfps = (sf - sf0) / s;
        sf0 = sf; sd0 = sd; ss0 = ss;
        if (++cycle % 2 == 1) lastG = gpu_pct(lastEng);   // every 10 s (the sample itself takes 0.5 s)
        char gb[96]; if (lastG >= 0) snprintf(gb, sizeof gb, "gpu %.0f%% (%s)", lastG, lastEng.c_str()); else snprintf(gb, sizeof gb, "gpu n/a");
        int w = hd ? hd->want_w : 0, h2 = hd ? hd->want_h : 0;
        ulog("[stats] %s %dx%d | phone %.1f fps | decode %.1f ms | crop/scale/write %.1f ms | cam->vcam %.0f ms | vcam->FrameServer avg %.1f max %.1f ms | to apps %.1f fps, repeats %lld | cpu feeder %.1f%% frameserver %.1f%% | %s",
             get_status().c_str(), w, h2, fr / s, fr ? dec / 1000.0 / fr : 0.0, fr ? wr / 1000.0 / fr : 0.0, g_stat_lat.load(), hand, hmax, sfps, (long long)sd, c, fcpu, gb);
    }
    if (fsp) CloseHandle(fsp);
}

// ---------- control server for the tray ----------
static void handle_client(SOCKET c) {
    DWORD t = 2000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    std::string line; char ch;
    while (line.size() < 300) { int k = recv(c, &ch, 1, 0); if (k <= 0) break; if (ch == '\n') break; line += ch; }
    std::string rep = "{\"ok\":false,\"err\":\"unknown command\"}";
    if (line.find("\"status\"") != std::string::npos) {
        std::string stt = get_status(), sub = get_sub();
        bool live = now_us() - g_last_live_us.load() < 1500000;
        for (char& q : stt) if (q == '"' || q == '\\') q = ' ';
        for (char& q : sub) if (q == '"' || q == '\\') q = ' ';
        UcvHeader* hd = g_sh.hdr;
        char b[700];
        snprintf(b, sizeof b, "{\"ok\":true,\"vcam\":%s,\"connected\":%s,\"apps\":%s,\"live\":%s,\"mirror\":%d,\"w\":%d,\"h\":%d,\"fps\":%.1f,\"lat_ms\":%.0f,\"status\":\"%s\",\"sub\":\"%s\"}",
                 g_vcam_on ? "true" : "false", hd ? "true" : "false", consumer_recent(2000) ? "true" : "false", live ? "true" : "false",
                 g_mirror.load(), hd ? (int)hd->want_w : 0, hd ? (int)hd->want_h : 0, g_stat_fps.load(), g_stat_lat.load(), stt.c_str(), sub.c_str());
        rep = b;
    } else if (line.find("\"mirror\"") != std::string::npos) {
        bool on = line.find("true") != std::string::npos || line.find(":1") != std::string::npos;
        g_mirror = on ? 1 : 0;
        if (g_sh.hdr) g_sh.hdr->mirror = g_mirror.load();
        ulog("[mirror] %s", on ? "on" : "off");
        rep = "{\"ok\":true}";
    } else if (line.find("\"quit\"") != std::string::npos) {
        rep = "{\"ok\":true}";
        g_run = false;
    }
    rep += "\n";
    send(c, rep.data(), (int)rep.size(), 0);
    closesocket(c);
}

static void ctl_server(SOCKET ls) {
    while (g_run) {
        fd_set fds; FD_ZERO(&fds); FD_SET(ls, &fds);
        timeval tv = { 0, 300000 };
        int n = select(0, &fds, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c != INVALID_SOCKET) handle_client(c);
    }
}

// ---------- virtual camera registration (Windows 11) ----------
static const GUID kCatVideoCamera = { 0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 } };
static const GUID kCatVideo       = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID kCatCapture     = { 0x65e8773d, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };

static DWORD win_build() {
    typedef LONG(WINAPI * Fn)(PRTL_OSVERSIONINFOW);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    Fn fn = nt ? (Fn)GetProcAddress(nt, "RtlGetVersion") : nullptr;
    RTL_OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof vi;
    if (fn && fn(&vi) == 0) return vi.dwBuildNumber;
    return 0;
}
static bool com_registered() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" UCV_CLSID_STR L"\\InprocServer32", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

static IMFVirtualCamera* g_vc = nullptr;
// creates or removes the "UsbCam Camera" virtual camera (main thread only)
static bool vcam_enable(bool on, std::string& err) {
    if (on) {
        if (g_vc) return true;
        IMFVirtualCamera* vc = nullptr;
        const GUID cats[3] = { kCatVideoCamera, kCatVideo, kCatCapture };
        HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser,
                                           UCV_CAMERA_NAME, UCV_CLSID_STR, cats, 3, &vc);
        if (SUCCEEDED(hr)) hr = vc->Start(nullptr);
        if (FAILED(hr)) {
            char b[120]; snprintf(b, sizeof b, "Cannot create the virtual camera (0x%08lx)", (unsigned long)hr);
            ulog("[error] %s", b); err = b;
            if (vc) vc->Release();
            return false;
        }
        g_vc = vc; g_vcam_on = true;
        ulog("[vcam] \"UsbCam Camera\" is available to apps");
        return true;
    }
    if (g_vc) {
        g_vcam_on = false;
        g_vc->Stop(); g_vc->Remove(); g_vc->Shutdown(); g_vc->Release(); g_vc = nullptr;
        ulog("[vcam] camera removed");
    }
    return true;
}

#ifdef UCV_GUI
#include "usbcam_gui.inc"
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto nxt = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--cam") g_cfg.cam = nxt();
        else if (a == "--w") g_cfg.w = atoi(nxt().c_str());
        else if (a == "--h") g_cfg.h = atoi(nxt().c_str());
        else if (a == "--fps") g_cfg.fps = atoi(nxt().c_str());
        else if (a == "--br") g_cfg.br = atoi(nxt().c_str());
        else if (a == "--ll") g_cfg.ll = atoi(nxt().c_str());
        else if (a == "--sw") g_cfg.sw = true;
        else if (a == "--seconds") g_cfg.seconds = atoi(nxt().c_str());
        else if (a == "--idle") g_cfg.idle_s = atoi(nxt().c_str());
        else if (a == "--mirror") g_cfg.mirror = atoi(nxt().c_str());
        else if (a == "--port") g_cfg.port = atoi(nxt().c_str());
        else { printf("usage: usbcam_vcam [--mirror 0|1] [--idle seconds] [--cam id --w n --h n --fps n --br bps --ll 0|1] [--sw] [--seconds n] [--port n]\n"); return 1; }
    }
    g_mirror = g_cfg.mirror ? 1 : 0;
    log_open();
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    HANDLE one = CreateMutexW(nullptr, FALSE, L"Local\\UsbCamVcamFeeder");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
#ifdef UCV_GUI
        // another copy (for example the tray's background program) is running: ask it to quit, then take over
        bool got = false;
        for (int i = 0; i < 12 && !got; i++) {
            if (i == 0 || i == 6) { SOCKET c = tcp_connect(g_cfg.port, 1000); if (c != INVALID_SOCKET) { std::string rr; ctl_cmd(c, "{\"cmd\":\"quit\"}", rr); closesocket(c); } }
            CloseHandle(one);
            sleep_ms(500);
            one = CreateMutexW(nullptr, FALSE, L"Local\\UsbCamVcamFeeder");
            if (one && GetLastError() != ERROR_ALREADY_EXISTS) got = true;
        }
        if (!got) { MessageBoxW(nullptr, L"UsbCam is already running.", L"UsbCam", MB_OK | MB_ICONINFORMATION); return 0; }
#else
        ulog("already running"); return 0;
#endif
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    DWORD build = win_build();
    ulog("usbcam_vcam start (Windows build %lu, mirror %d, idle %d s)", build, g_mirror.load(), g_cfg.idle_s);
    g_vcam_supported = build >= 22000 && com_registered();
    if (build < 22000) g_vcam_why = "The virtual camera needs Windows 11. The preview still works.";
    else if (!com_registered()) g_vcam_why = "The virtual camera is not installed yet. Run pc\\vcam-install.ps1 once.";
#ifndef UCV_GUI
    if (build < 22000) { ulog("[error] the virtual camera needs Windows 11 (build 22000 or newer)"); return 4; }
    if (!com_registered()) { ulog("[error] camera component not installed: run pc\\vcam-install.ps1 first"); return 3; }
#endif
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) { ulog("[error] MFStartup failed"); return 2; }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in la = {}; la.sin_family = AF_INET; la.sin_port = htons((uint16_t)g_cfg.port); inet_pton(AF_INET, "127.0.0.1", &la.sin_addr);
    bool ctl_ok = !(bind(ls, (sockaddr*)&la, sizeof la) != 0 || listen(ls, 4) != 0);
    if (!ctl_ok) { ulog("[error] control port %d is busy", g_cfg.port);
#ifndef UCV_GUI
        return 5;
#endif
    }

#ifndef UCV_GUI
    { std::string e; if (!vcam_enable(true, e)) return 6; }
#endif

    set_status("idle (no app is using the camera)", "Waiting for an app to open the camera");
    std::thread t_open(sh_open_thread), t_ph(placeholder_thread), t_net(net_main), t_st(stats_thread), t_ctl([&] { if (ctl_ok) ctl_server(ls); });
#ifdef UCV_GUI
    gui_loop();   // returns when the window is closed
    g_run = false;
#else
    int64_t t_start = now_us();
    while (g_run) {
        sleep_ms(200);
        if (g_cfg.seconds && now_us() - t_start > g_cfg.seconds * 1000000LL) g_run = false;
    }
#endif
    SOCKET v = g_vid.load(); if (v != INVALID_SOCKET) shutdown(v, SD_BOTH);
    t_net.join(); t_ph.join(); t_st.join(); t_open.join(); t_ctl.join();
    closesocket(ls);
    g_vcam_on = false;
    { std::string e; vcam_enable(false, e); }
    MFShutdown();
    ulog("usbcam_vcam stopped, camera removed");
    return 0;
}
