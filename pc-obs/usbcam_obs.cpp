// usbcam-obs: UsbCam OBS source plugin (M4).
// Same receive path as pc-recv/usbcam_view.cpp (adb forward -> control channel -> framed H.264 -> D3D11VA decode),
// but frames go to OBS as an UNBUFFERED async video source (that is what removes the ~600 ms of Media Source buffering).
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_BAD INVALID_SOCKET
  #define CLOSESOCK closesocket
  #define SHUT_BOTH SD_BOTH
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_BAD (-1)
  #define CLOSESOCK close
  #define SHUT_BOTH SHUT_RDWR
#endif

#include <obs-module.h>
#include <util/platform.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char* obs_module_description(void) { return "UsbCam: Android phone camera over USB (adb), low latency"; }

#define LOG(lvl, fmt, ...) blog(lvl, "[usbcam] " fmt, ##__VA_ARGS__)

using Clock = std::chrono::steady_clock;
static int64_t now_us() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }

struct Cfg {
    std::string adb = "adb", cam;
    int w = 0, h = 0, fps = 0, br_mbps = 0, cs = 0;   // 0 = phone default / auto
    bool hw = true, keep = false;
};

struct Src {
    obs_source_t* src = nullptr;
    Cfg cfg;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<sock_t> vid{(sock_t)SOCK_BAD};
    std::atomic<bool> visible{false};
    std::mutex smu; std::string status = "idle";
    std::string hwname = "?";
    // clock sync (phone clock -> our steady clock)
    int64_t off = 0, off2 = 0; bool off_ok = false, off2_ok = false; int dom = 0;
};

static void set_status(Src* s, const std::string& st) {
    std::lock_guard<std::mutex> l(s->smu);
    if (st != s->status) { s->status = st; LOG(LOG_INFO, "%s", st.c_str()); }
}
static void sleep_run(Src* s, int ms) { for (int i = 0; i < ms / 20 && s->run; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20)); }

// ---------- process / network helpers ----------
static std::string run_out(const std::string& cmdline) {
    std::string out;
#ifdef _WIN32
    // no console window flashing inside OBS, and a hard timeout so a stuck adb can never hang OBS
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdline;
    BOOL ok = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (ok) {
        DWORD t0 = GetTickCount();
        for (;;) {
            DWORD avail = 0; char b[512];
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                DWORD n = 0; if (!ReadFile(rd, b, sizeof b, &n, nullptr) || !n) break;
                out.append(b, n);
            }
            if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
                while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                    DWORD n = 0; if (!ReadFile(rd, b, sizeof b, &n, nullptr) || !n) break;
                    out.append(b, n);
                }
                break;
            }
            if (GetTickCount() - t0 > 10000) { TerminateProcess(pi.hProcess, 1); break; }
        }
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
#else
    FILE* p = popen((cmdline + " 2>&1").c_str(), "r");
    if (!p) return out;
    char b[256]; while (fgets(b, sizeof b, p)) out += b;
    pclose(p);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}
static std::string adb(Src* s, const std::string& args) { return run_out("\"" + s->cfg.adb + "\" " + args); }

static sock_t tcp_connect(int port, int rcv_ms) {
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) return SOCK_BAD;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { CLOSESOCK(s); return SOCK_BAD; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef _WIN32
    DWORD t = rcv_ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
#else
    timeval tv{rcv_ms / 1000, (rcv_ms % 1000) * 1000}; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
    return s;
}
static bool read_n(sock_t s, uint8_t* b, size_t n) {
    size_t o = 0;
    while (o < n) { int k = recv(s, (char*)b + o, (int)(n - o), 0); if (k <= 0) return false; o += (size_t)k; }
    return true;
}
static bool ctl_cmd(sock_t s, const std::string& json, std::string& reply) {
    std::string m = json + "\n";
    if (send(s, m.data(), (int)m.size(), 0) <= 0) return false;
    reply.clear(); char c;
    while (true) { int k = recv(s, &c, 1, 0); if (k <= 0) return false; if (c == '\n') return true; reply += c; }
}
static int64_t json_int(const std::string& s, const char* key) {
    size_t p = s.find(key); if (p == std::string::npos) return 0; return atoll(s.c_str() + p + strlen(key));
}
static bool adb_ready(Src* s) {
    if (adb(s, "get-state") != "device") return false;
    adb(s, "forward tcp:5000 tcp:5000");
    adb(s, "forward tcp:5002 tcp:5002");
    return true;
}

// ---------- decoder ----------
struct Dec { AVCodecContext* c = nullptr; AVBufferRef* hw = nullptr; AVFrame *f = nullptr, *sw = nullptr; AVPacket* p = nullptr; };

static enum AVPixelFormat get_fmt(AVCodecContext* c, const enum AVPixelFormat* f) {
    for (const enum AVPixelFormat* q = f; *q != AV_PIX_FMT_NONE; q++) if (*q == AV_PIX_FMT_D3D11) return *q;
    return avcodec_default_get_format(c, f);
}
static bool dec_open(Src* s, Dec& d) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    d.c = avcodec_alloc_context3(codec);
    d.c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    d.c->flags2 |= AV_CODEC_FLAG2_FAST;
    d.c->thread_count = 1;
    s->hwname = "software";
    if (s->cfg.hw && av_hwdevice_ctx_create(&d.hw, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
        d.c->hw_device_ctx = av_buffer_ref(d.hw);
        d.c->get_format = get_fmt;
        s->hwname = "d3d11va";
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

// hand a decoded frame to OBS (unbuffered async video)
static bool output(Src* s, Dec& d) {
    AVFrame* out = d.f;
    if (d.f->format == AV_PIX_FMT_D3D11) {
        av_frame_unref(d.sw);
        if (av_hwframe_transfer_data(d.sw, d.f, 0) < 0) { LOG(LOG_WARNING, "hw->sw transfer failed"); return false; }
        out = d.sw;
    }
    obs_source_frame fr = {};
    if (out->format == AV_PIX_FMT_NV12) fr.format = VIDEO_FORMAT_NV12;
    else if (out->format == AV_PIX_FMT_YUV420P) fr.format = VIDEO_FORMAT_I420;
    else { static bool w = false; if (!w) { LOG(LOG_WARNING, "unsupported pixel format %d", (int)out->format); w = true; } return false; }
    for (int i = 0; i < 3; i++) { fr.data[i] = out->data[i]; fr.linesize[i] = (uint32_t)out->linesize[i]; }
    fr.width = (uint32_t)out->width; fr.height = (uint32_t)out->height;
    fr.timestamp = os_gettime_ns();
    bool full = d.f->color_range == AVCOL_RANGE_JPEG;
    video_colorspace cs;
    if (s->cfg.cs == 1) cs = VIDEO_CS_601;
    else if (s->cfg.cs == 2) cs = VIDEO_CS_709;
    else if (d.f->colorspace == AVCOL_SPC_SMPTE170M || d.f->colorspace == AVCOL_SPC_BT470BG) cs = VIDEO_CS_601;
    else if (d.f->colorspace == AVCOL_SPC_BT709) cs = VIDEO_CS_709;
    else cs = out->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
    fr.full_range = full;
    video_format_get_parameters_for_format(cs, full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, fr.format, fr.color_matrix,
                                           fr.color_range_min, fr.color_range_max);
    obs_source_output_video(s->src, &fr);   // copies the data, so our buffers can be reused straight away
    return true;
}

// ---------- one phone session ----------
static void session(Src* s) {
    sock_t ctl = SOCK_BAD; std::string r;
    for (int i = 0; i < 8 && s->run; i++) {
        ctl = tcp_connect(5002, 3000);
        if (ctl != SOCK_BAD) {
            if (ctl_cmd(ctl, "{\"cmd\":\"hello\"}", r) && r.find("\"ok\":true") != std::string::npos) break;
            CLOSESOCK(ctl); ctl = SOCK_BAD;
        }
        if (i == 0) { set_status(s, "starting UsbCam on the phone (keep it unlocked)"); adb(s, "shell am start -n com.harman.usbcam/.MainActivity"); }
        sleep_run(s, 1200);
    }
    if (ctl == SOCK_BAD) { if (s->run) set_status(s, "cannot reach the UsbCam app (installed? opened once?)"); return; }

    int64_t best = INT64_MAX, off = 0, off2 = 0; bool has2 = false;
    for (int i = 0; i < 6; i++) {
        int64_t t0 = now_us();
        if (!ctl_cmd(ctl, "{\"cmd\":\"time\"}", r)) break;
        int64_t t1 = now_us(), p = json_int(r, "\"t_us\":"), pm = json_int(r, "\"mono_us\":");
        if (p && t1 - t0 < best) { best = t1 - t0; off = (t0 + t1) / 2 - p; if (pm) { off2 = (t0 + t1) / 2 - pm; has2 = true; } }
    }
    s->off = off; s->off_ok = best != INT64_MAX; s->off2 = off2; s->off2_ok = has2; s->dom = 0;

    const Cfg& c = s->cfg;
    std::string st = "{\"cmd\":\"start\",\"hdr\":true";
    if (!c.cam.empty()) st += ",\"cam\":\"" + c.cam + "\"";
    if (c.w) st += ",\"w\":" + std::to_string(c.w);
    if (c.h) st += ",\"h\":" + std::to_string(c.h);
    if (c.fps) st += ",\"fps\":" + std::to_string(c.fps);
    if (c.br_mbps) st += ",\"br\":" + std::to_string(c.br_mbps * 1000000);
    st += "}";
    if (!ctl_cmd(ctl, st, r) || r.find("\"ok\":true") == std::string::npos) {
        LOG(LOG_WARNING, "start failed: %s", r.c_str());
        set_status(s, "phone refused start"); CLOSESOCK(ctl); return;
    }
    size_t a = r.find("\"actual\":");
    if (a != std::string::npos) LOG(LOG_INFO, "phone %s", r.c_str() + a);

    sock_t vid = SOCK_BAD;
    for (int i = 0; i < 6 && s->run && vid == SOCK_BAD; i++) { vid = tcp_connect(5000, 4000); if (vid == SOCK_BAD) sleep_run(s, 400); }
    if (vid == SOCK_BAD) { set_status(s, "cannot open video port"); CLOSESOCK(ctl); return; }
    s->vid = vid;

    Dec d;
    if (!dec_open(s, d)) { set_status(s, "decoder open failed"); dec_close(d); CLOSESOCK(vid); s->vid = SOCK_BAD; CLOSESOCK(ctl); return; }
    set_status(s, std::string("streaming (") + s->hwname + ")");

    std::vector<uint8_t> buf(1 << 20);
    uint8_t h[13];
    int64_t t_stat = now_us(), dec_us = 0, lat_sum = 0; int n_dec = 0, n_lat = 0;
    while (s->run) {
        if (!read_n(vid, h, 13)) break;
        uint32_t len = ((uint32_t)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        int64_t pts = 0; for (int i = 4; i < 12; i++) pts = (pts << 8) | h[i];
        if (len == 0 || len > 16u * 1024 * 1024) { LOG(LOG_WARNING, "bad frame length %u", len); break; }
        if (buf.size() < len + AV_INPUT_BUFFER_PADDING_SIZE) buf.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!read_n(vid, buf.data(), len)) break;
        memset(buf.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        int64_t t0 = now_us();
        d.p->data = buf.data(); d.p->size = (int)len; d.p->pts = pts; d.p->dts = pts;
        if (avcodec_send_packet(d.c, d.p) < 0) continue;
        if (h[12] & 2) continue;  // codec config only
        while (avcodec_receive_frame(d.c, d.f) == 0) {
            if (!output(s, d)) continue;
            int64_t n = now_us(); n_dec++; dec_us += n - t0;
            if (s->off_ok) {   // which phone clock do the frame timestamps use? (decided on the first frame)
                double lb = (n - (pts + s->off)) / 1000.0;
                double lm = s->off2_ok ? (n - (pts + s->off2)) / 1000.0 : -1e9;
                if (s->dom == 0) {
                    s->dom = (lm > -50 && lm < 3000) ? 2 : (lb > -50 && lb < 3000) ? 1 : 3;
                    LOG(LOG_INFO, "frame timestamps use the phone's %s clock", s->dom == 2 ? "monotonic" : s->dom == 1 ? "elapsedRealtime" : "UNKNOWN");
                }
                if (s->dom == 1 || s->dom == 2) { lat_sum += (int64_t)(s->dom == 2 ? lm : lb); n_lat++; }
            }
        }
        int64_t now = now_us();
        if (now - t_stat >= 5000000) {
            double sec = (now - t_stat) / 1e6;
            LOG(LOG_INFO, "%.1f fps | decode+output %.1f ms/frame | camera->OBS %lld ms (excludes OBS render)", n_dec / sec,
                n_dec ? dec_us / 1000.0 / n_dec : 0.0, (long long)(n_lat ? lat_sum / n_lat : 0));
            n_dec = 0; dec_us = 0; lat_sum = 0; n_lat = 0; t_stat = now;
        }
    }
    dec_close(d);
    std::string dummy;
    if (!s->run) ctl_cmd(ctl, "{\"cmd\":\"stop\"}", dummy);
    CLOSESOCK(vid); s->vid = SOCK_BAD; CLOSESOCK(ctl);
    obs_source_output_video(s->src, nullptr);   // clear the picture instead of freezing on the last frame
    if (s->run) set_status(s, "connection lost, retrying");
}

static void net_main(Src* s) {
    while (s->run) {
        set_status(s, "waiting for phone (adb)");
        if (!adb_ready(s)) { sleep_run(s, 1000); continue; }
        session(s);
        if (s->run) sleep_run(s, 800);
    }
    set_status(s, "idle");
}

static void src_start(Src* s) { if (s->th.joinable()) return; s->run = true; s->th = std::thread(net_main, s); }
static void src_stop(Src* s) {
    s->run = false;
    sock_t v = s->vid.load(); if (v != SOCK_BAD) shutdown(v, SHUT_BOTH);   // wakes a blocked recv
    if (s->th.joinable()) s->th.join();
}
static void src_apply(Src* s) { if (s->visible || s->cfg.keep) src_start(s); else src_stop(s); }

// ---------- OBS glue ----------
static const char* get_name(void*) { return "UsbCam (USB phone camera)"; }

static void read_cfg(Src* s, obs_data_t* d) {
    Cfg& c = s->cfg;
    c.adb = obs_data_get_string(d, "adb"); if (c.adb.empty()) c.adb = "adb";
    c.cam = obs_data_get_string(d, "cam");
    c.w = (int)obs_data_get_int(d, "w"); c.h = (int)obs_data_get_int(d, "h");
    c.fps = (int)obs_data_get_int(d, "fps"); c.br_mbps = (int)obs_data_get_int(d, "br");
    c.cs = (int)obs_data_get_int(d, "cs");
    c.hw = obs_data_get_bool(d, "hw"); c.keep = obs_data_get_bool(d, "keep");
}

static void* src_create(obs_data_t* settings, obs_source_t* source) {
    Src* s = new Src();
    s->src = source;
#ifdef _WIN32
    static std::once_flag once; std::call_once(once, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
#endif
    obs_source_set_async_unbuffered(source, true);   // show frames as soon as they arrive: the whole point of this plugin
    read_cfg(s, settings);
    s->visible = obs_source_showing(source);
    src_apply(s);
    return s;
}
static void src_destroy(void* p) { Src* s = (Src*)p; src_stop(s); delete s; }
static void src_update(void* p, obs_data_t* settings) {
    Src* s = (Src*)p;
    src_stop(s);          // restart the session with the new settings
    read_cfg(s, settings);
    src_apply(s);
}
static void src_show(void* p) { Src* s = (Src*)p; s->visible = true; src_apply(s); }
static void src_hide(void* p) { Src* s = (Src*)p; s->visible = false; src_apply(s); }

static void src_defaults(obs_data_t* d) {
    obs_data_set_default_string(d, "adb", "adb");
    obs_data_set_default_int(d, "w", 0); obs_data_set_default_int(d, "h", 0);
    obs_data_set_default_int(d, "fps", 0); obs_data_set_default_int(d, "br", 0);
    obs_data_set_default_int(d, "cs", 0);
    obs_data_set_default_bool(d, "hw", true); obs_data_set_default_bool(d, "keep", false);
}

static obs_properties_t* src_props(void* p) {
    Src* s = (Src*)p;
    obs_properties_t* pr = obs_properties_create();
    std::string st; if (s) { std::lock_guard<std::mutex> l(s->smu); st = s->status; }
    obs_properties_add_text(pr, "info", ("Status: " + (st.empty() ? std::string("-") : st) +
        "\nPlug in the phone (USB debugging on). 0 = phone default (1080p30 on the main camera).").c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(pr, "cam", "Camera id (blank = default)", OBS_TEXT_DEFAULT);
    obs_properties_add_int(pr, "w", "Width (0 = auto)", 0, 8192, 1);
    obs_properties_add_int(pr, "h", "Height (0 = auto)", 0, 8192, 1);
    obs_properties_add_int(pr, "fps", "FPS (0 = auto)", 0, 120, 1);
    obs_properties_add_int(pr, "br", "Bitrate Mbit/s (0 = auto)", 0, 200, 1);
    obs_property_t* cs = obs_properties_add_list(pr, "cs", "Colour matrix", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(cs, "Auto", 0); obs_property_list_add_int(cs, "BT.601", 1); obs_property_list_add_int(cs, "BT.709", 2);
    obs_properties_add_bool(pr, "hw", "Hardware decoding (D3D11VA)");
    obs_properties_add_bool(pr, "keep", "Stay connected while hidden");
    obs_properties_add_text(pr, "adb", "adb path", OBS_TEXT_DEFAULT);
    return pr;
}

static obs_source_info g_info = {};

bool obs_module_load(void) {
    g_info.id = "usbcam_source";
    g_info.type = OBS_SOURCE_TYPE_INPUT;
    g_info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    g_info.get_name = get_name;
    g_info.create = src_create;
    g_info.destroy = src_destroy;
    g_info.update = src_update;
    g_info.get_defaults = src_defaults;
    g_info.get_properties = src_props;
    g_info.show = src_show;
    g_info.hide = src_hide;
    g_info.icon_type = OBS_ICON_TYPE_CAMERA;
    obs_register_source(&g_info);
    return true;
}
