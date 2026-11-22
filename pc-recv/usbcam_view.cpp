// usbcam_view: UsbCam PC receiver (M3).
// adb forward + control channel (start, hdr=true) -> framed H.264 -> hardware decode (D3D11VA, software fallback)
// -> newest-frame-wins display in an SDL window. Prints measured camera->screen latency.
#define SDL_MAIN_HANDLED
#include <SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_BAD INVALID_SOCKET
  #define CLOSESOCK closesocket
  #define SHUT_BOTH SD_BOTH
  #define POPEN _popen
  #define PCLOSE _pclose
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
  #define POPEN popen
  #define PCLOSE pclose
#endif

using Clock = std::chrono::steady_clock;
static int64_t now_us() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct Cfg { std::string cam; int w = 0, h = 0, fps = 0, br = 0; bool sw = false; int seconds = 0; int ll = -1; } g_cfg;

static std::atomic<bool> g_run{true};
static std::atomic<sock_t> g_vid{(sock_t)SOCK_BAD};
static std::atomic<int64_t> g_off{0};          // pc_us = phone_us + g_off
static std::atomic<int64_t> g_off2{0};         // same, for the phone's monotonic clock (mono_us)
static std::atomic<bool> g_off_ok{false}, g_off2_ok{false};
static std::atomic<int> g_dom{0};              // pts clock domain: 0 unknown, 1 elapsedRealtime, 2 monotonic, 3 neither fits
static std::mutex g_mu;                        // guards the latest-frame slot + status
static std::condition_variable g_cv;
static AVFrame* g_latest = nullptr;
static int64_t g_latest_pts = 0;
static bool g_new = false;
static std::string g_status = "starting";
static std::string g_hw = "?";
static std::atomic<int> g_decoded{0}, g_overwritten{0};
static std::atomic<int64_t> g_dec_us{0};

static void set_status(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mu);
    if (s != g_status) { g_status = s; printf("[status] %s\n", s.c_str()); }
}

// ---------- process / network helpers ----------
static std::string run_out(const std::string& cmd) {
    std::string out;
    FILE* p = POPEN(cmd.c_str(), "r");
    if (!p) return out;
    char b[256];
    while (fgets(b, sizeof b, p)) out += b;
    PCLOSE(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

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
    while (o < n) {
        int k = recv(s, (char*)b + o, (int)(n - o), 0);
        if (k <= 0) return false;
        o += (size_t)k;
    }
    return true;
}

static bool ctl_cmd(sock_t s, const std::string& json, std::string& reply) {
    std::string m = json + "\n";
    if (send(s, m.data(), (int)m.size(), 0) <= 0) return false;
    reply.clear();
    char c;
    while (true) {
        int k = recv(s, &c, 1, 0);
        if (k <= 0) return false;
        if (c == '\n') return true;
        reply += c;
    }
}

static bool adb_ready() {
    if (run_out("adb get-state 2>&1") != "device") return false;
    run_out("adb forward tcp:5000 tcp:5000 2>&1");
    run_out("adb forward tcp:5002 tcp:5002 2>&1");
    return true;
}

// ---------- decoder ----------
struct Dec { AVCodecContext* c = nullptr; AVBufferRef* hw = nullptr; AVFrame *f = nullptr, *sw = nullptr; AVPacket* p = nullptr; };

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

static void publish(Dec& d) {
    AVFrame* out = d.f;
    if (d.f->format == AV_PIX_FMT_D3D11) {
        av_frame_unref(d.sw);
        if (av_hwframe_transfer_data(d.sw, d.f, 0) < 0) { printf("[warn] hw->sw transfer failed\n"); return; }
        out = d.sw;
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (g_new) g_overwritten++;
    av_frame_unref(g_latest);
    av_frame_ref(g_latest, out);
    g_latest_pts = d.f->pts;
    g_new = true;
    g_cv.notify_one();
}

// ---------- one phone session ----------
static int64_t json_int(const std::string& s, const char* key) {
    size_t p = s.find(key);
    if (p == std::string::npos) return 0;
    return atoll(s.c_str() + p + strlen(key));
}

static void session() {
    sock_t ctl = SOCK_BAD; std::string r;
    for (int i = 0; i < 8 && g_run; i++) {
        ctl = tcp_connect(5002, 3000);
        if (ctl != SOCK_BAD) {
            if (ctl_cmd(ctl, "{\"cmd\":\"hello\"}", r) && r.find("\"ok\":true") != std::string::npos) break;
            CLOSESOCK(ctl); ctl = SOCK_BAD;
        }
        if (i == 0) { set_status("starting UsbCam on the phone (keep it unlocked)"); run_out("adb shell am start -n com.harman.usbcam/.MainActivity 2>&1"); }
        sleep_ms(1200);
    }
    if (ctl == SOCK_BAD) { set_status("cannot reach the UsbCam app (is it installed? open it once)"); return; }

    // clock sync: phone `time` vs our clock, keep the smallest round trip
    int64_t best = INT64_MAX, off = 0, off2 = 0; bool has2 = false;
    for (int i = 0; i < 6; i++) {
        int64_t t0 = now_us();
        if (!ctl_cmd(ctl, "{\"cmd\":\"time\"}", r)) break;
        int64_t t1 = now_us(), p = json_int(r, "\"t_us\":");
        int64_t pm = json_int(r, "\"mono_us\":");
        if (p && t1 - t0 < best) { best = t1 - t0; off = (t0 + t1) / 2 - p; if (pm) { off2 = (t0 + t1) / 2 - pm; has2 = true; } }
    }
    g_off = off; g_off_ok = best != INT64_MAX; g_off2 = off2; g_off2_ok = has2; g_dom = 0;
    if (g_off_ok) printf("[sync] control round trip %.1f ms\n", best / 1000.0);

    std::string st = "{\"cmd\":\"start\",\"hdr\":true";
    if (!g_cfg.cam.empty()) st += ",\"cam\":\"" + g_cfg.cam + "\"";
    if (g_cfg.w) st += ",\"w\":" + std::to_string(g_cfg.w);
    if (g_cfg.h) st += ",\"h\":" + std::to_string(g_cfg.h);
    if (g_cfg.fps) st += ",\"fps\":" + std::to_string(g_cfg.fps);
    if (g_cfg.br) st += ",\"br\":" + std::to_string(g_cfg.br);
    if (g_cfg.ll >= 0) st += ",\"ll\":" + std::to_string(g_cfg.ll);
    st += "}";
    if (!ctl_cmd(ctl, st, r) || r.find("\"ok\":true") == std::string::npos) {
        printf("[error] start failed: %s\n", r.c_str());
        set_status("phone refused start"); CLOSESOCK(ctl); return;
    }
    size_t a = r.find("\"actual\":");
    if (a != std::string::npos) printf("[phone] %s\n", r.c_str() + a);

    sock_t vid = SOCK_BAD;
    for (int i = 0; i < 6 && g_run && vid == SOCK_BAD; i++) { vid = tcp_connect(5000, 4000); if (vid == SOCK_BAD) sleep_ms(400); }
    if (vid == SOCK_BAD) { set_status("cannot open video port"); CLOSESOCK(ctl); return; }
    g_vid = vid;

    Dec d;
    if (!dec_open(d)) { set_status("decoder open failed"); CLOSESOCK(vid); CLOSESOCK(ctl); return; }
    set_status(std::string("streaming (") + g_hw + ")");

    std::vector<uint8_t> buf(1 << 20);
    uint8_t h[13];
    while (g_run) {
        if (!read_n(vid, h, 13)) break;
        uint32_t len = ((uint32_t)h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3];
        int64_t pts = 0; for (int i = 4; i < 12; i++) pts = (pts << 8) | h[i];
        if (len == 0 || len > 16u * 1024 * 1024) { printf("[error] bad frame length %u\n", len); break; }
        if (buf.size() < len + AV_INPUT_BUFFER_PADDING_SIZE) buf.resize(len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!read_n(vid, buf.data(), len)) break;
        memset(buf.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        int64_t t0 = now_us();
        d.p->data = buf.data(); d.p->size = (int)len; d.p->pts = pts; d.p->dts = pts;
        if (avcodec_send_packet(d.c, d.p) < 0) continue;
        if (h[12] & 2) continue;  // codec config only (SPS/PPS)
        while (avcodec_receive_frame(d.c, d.f) == 0) { publish(d); g_decoded++; }
        g_dec_us += now_us() - t0;
    }
    dec_close(d);
    std::string dummy;
    if (!g_run) { ctl_cmd(ctl, "{\"cmd\":\"stop\"}", dummy); }
    CLOSESOCK(vid); g_vid = SOCK_BAD; CLOSESOCK(ctl);
    if (g_run) set_status("connection lost, retrying");
}

static void net_main() {
    while (g_run) {
        set_status("waiting for phone (adb)");
        if (!adb_ready()) { sleep_ms(1000); continue; }
        session();
        if (g_run) sleep_ms(800);
    }
}

// ---------- main: window + newest-frame render ----------
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
        else if (a == "--sw") g_cfg.sw = true;
        else if (a == "--seconds") g_cfg.seconds = atoi(nxt().c_str());
        else if (a == "--ll") g_cfg.ll = atoi(nxt().c_str());
        else { printf("usage: usbcam_view [--cam id] [--w n --h n --fps n --br bps] [--sw] [--seconds n] [--ll 0|1]\n"); return 1; }
    }
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { printf("SDL init failed: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* win = SDL_CreateWindow("UsbCam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!win || !ren) { printf("window/renderer failed: %s\n", SDL_GetError()); return 1; }
    g_latest = av_frame_alloc();
    AVFrame* show = av_frame_alloc();

    std::thread net(net_main);
    SDL_Texture* tex = nullptr; int tw = 0, th = 0, tfmt = -1;
    int64_t t_start = now_us(), t_stat = t_start, shown = 0, lat_n = 0; double lat_sum = 0, lat_last = 0;
    int dec0 = 0, ow0 = 0; int64_t decus0 = 0;

    while (g_run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) g_run = false;
            if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)) g_run = false;
        }
        if (g_cfg.seconds && now_us() - t_start > g_cfg.seconds * 1000000LL) g_run = false;
        bool have = false; int64_t pts = 0;
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait_for(l, std::chrono::milliseconds(8), [] { return g_new; });
            if (g_new) { av_frame_unref(show); av_frame_move_ref(show, g_latest); pts = g_latest_pts; g_new = false; have = true; }
        }
        if (have && show->width > 0) {
            int fmt = show->format;
            if (fmt != AV_PIX_FMT_NV12 && fmt != AV_PIX_FMT_YUV420P) { static bool w = false; if (!w) { printf("[warn] unsupported pixel format %d\n", fmt); w = true; } }
            else {
                if (!tex || tw != show->width || th != show->height || tfmt != fmt) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, fmt == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV,
                                            SDL_TEXTUREACCESS_STREAMING, show->width, show->height);
                    tw = show->width; th = show->height; tfmt = fmt;
                    SDL_RenderSetLogicalSize(ren, tw, th);
                    printf("[video] %dx%d %s\n", tw, th, fmt == AV_PIX_FMT_NV12 ? "NV12" : "YUV420P");
                }
                if (fmt == AV_PIX_FMT_NV12) SDL_UpdateNVTexture(tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1]);
                else SDL_UpdateYUVTexture(tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1], show->data[2], show->linesize[2]);
                SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);
                shown++;
                if (g_off_ok) {
                    int64_t n = now_us();
                    double lb = (n - (pts + g_off.load())) / 1000.0;
                    double lm = g_off2_ok ? (n - (pts + g_off2.load())) / 1000.0 : -1e9;
                    if (g_dom == 0) {  // first frame decides which phone clock the timestamps use
                        if (lm > -50 && lm < 3000) g_dom = 2; else if (lb > -50 && lb < 3000) g_dom = 1; else g_dom = 3;
                        printf("[clock] frame timestamps use the phone's %s clock (elapsedRealtime-based %.0f ms, monotonic-based %.0f ms)\n",
                               g_dom == 2 ? "monotonic" : g_dom == 1 ? "elapsedRealtime" : "UNKNOWN", lb, lm);
                    }
                    if (g_dom == 1 || g_dom == 2) { lat_last = g_dom == 2 ? lm : lb; lat_sum += lat_last; lat_n++; }
                }
            }
        }
        int64_t now = now_us();
        if (now - t_stat >= 2000000) {
            double s = (now - t_stat) / 1e6;
            int dec = g_decoded - dec0, ow = g_overwritten - ow0; int64_t du = g_dec_us - decus0;
            printf("[stats] shown %.1f fps | decoded %.1f fps | decode %.1f ms/frame | skipped %d | cam->screen %.0f ms (avg %.0f)\n",
                   shown / s, dec / s, dec ? du / 1000.0 / dec : 0.0, ow, lat_last, lat_n ? lat_sum / lat_n : 0.0);
            char title[256]; { std::lock_guard<std::mutex> l(g_mu); snprintf(title, sizeof title, "UsbCam | %s | %s | %.0f ms", g_status.c_str(), g_hw.c_str(), lat_last); }
            SDL_SetWindowTitle(win, title);
            shown = 0; lat_sum = 0; lat_n = 0; dec0 = g_decoded; ow0 = g_overwritten; decus0 = g_dec_us; t_stat = now;
        }
    }
    sock_t v = g_vid.load(); if (v != SOCK_BAD) shutdown(v, SHUT_BOTH);
    net.join();
    if (tex) SDL_DestroyTexture(tex);
    av_frame_free(&show); av_frame_free(&g_latest);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
