// usbcam_gui.inc: the UsbCam window (live preview + controls). Included by usbcam_vcam.cpp when UCV_GUI is defined.
// Uses SDL2 for the window and Dear ImGui (MIT) for the controls. Original code.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <algorithm>

// ---------- tiny JSON reader (phone replies) ----------
struct JV {
    char t = 0;   // o object, a array, s string, n number, b bool, z null
    double n = 0; std::string s;
    std::vector<JV> a; std::vector<std::pair<std::string, JV>> o;
    const JV* get(const char* k) const { for (auto& kv : o) if (kv.first == k) return &kv.second; return nullptr; }
    std::string str(const char* k) const { auto* v = get(k); return v && v->t == 's' ? v->s : std::string(); }
    double num(const char* k, double d = 0) const { auto* v = get(k); return v && (v->t == 'n' || v->t == 'b') ? v->n : d; }
};
static void jws(const char*& p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; }
static bool jparse(const char*& p, JV& v) {
    jws(p);
    if (*p == '{') {
        v.t = 'o'; p++; jws(p);
        if (*p == '}') { p++; return true; }
        for (;;) {
            jws(p); JV k; if (*p != '"' || !jparse(p, k)) return false;
            jws(p); if (*p != ':') return false; p++;
            JV val; if (!jparse(p, val)) return false;
            v.o.emplace_back(k.s, std::move(val)); jws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
    }
    if (*p == '[') {
        v.t = 'a'; p++; jws(p);
        if (*p == ']') { p++; return true; }
        for (;;) {
            JV val; if (!jparse(p, val)) return false;
            v.a.push_back(std::move(val)); jws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
    }
    if (*p == '"') {
        v.t = 's'; p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { p++; char c = *p; v.s += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'u' ? '?' : c; if (c == 'u') p += 4; p++; }
            else v.s += *p++;
        }
        if (*p != '"') return false;
        p++; return true;
    }
    if (!strncmp(p, "true", 4)) { v.t = 'b'; v.n = 1; p += 4; return true; }
    if (!strncmp(p, "false", 5)) { v.t = 'b'; p += 5; return true; }
    if (!strncmp(p, "null", 4)) { v.t = 'z'; p += 4; return true; }
    char* e = nullptr; v.n = strtod(p, &e); if (e == p) return false;
    v.t = 'n'; p = e; return true;
}
static bool jparse_all(const std::string& txt, JV& v) { const char* p = txt.c_str(); return jparse(p, v); }

// ---------- saved settings (%APPDATA%\UsbCam\app.ini) ----------
struct GuiSettings {
    bool vcam = true, mirror = false, phone_default = true;
    std::string cam; int w = 0, h = 0, fps = 0, br = 0;   // br in bit/s, 0 = automatic
};
static std::string settings_path() {
    const char* a = getenv("APPDATA");
    std::string d = std::string(a ? a : ".") + "\\UsbCam";
    CreateDirectoryA(d.c_str(), nullptr);
    return d + "\\app.ini";
}
static void settings_load(GuiSettings& s) {
    FILE* f = fopen(settings_path().c_str(), "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (k == "vcam") s.vcam = v == "1";
        else if (k == "mirror") s.mirror = v == "1";
        else if (k == "phone_default") s.phone_default = v == "1";
        else if (k == "cam") s.cam = v;
        else if (k == "w") s.w = atoi(v.c_str());
        else if (k == "h") s.h = atoi(v.c_str());
        else if (k == "fps") s.fps = atoi(v.c_str());
        else if (k == "br") s.br = atoi(v.c_str());
    }
    fclose(f);
}
static void settings_save(const GuiSettings& s) {
    FILE* f = fopen(settings_path().c_str(), "w");
    if (!f) return;
    fprintf(f, "vcam=%d\nmirror=%d\nphone_default=%d\ncam=%s\nw=%d\nh=%d\nfps=%d\nbr=%d\n",
            s.vcam ? 1 : 0, s.mirror ? 1 : 0, s.phone_default ? 1 : 0, s.cam.c_str(), s.w, s.h, s.fps, s.br);
    fclose(f);
}

// send the picture settings to the phone session (it restarts with them)
static void gui_apply(const GuiSettings& s, bool restart) {
    {
        std::lock_guard<std::mutex> l(g_cfgmu);
        if (s.phone_default) { g_cfg.cam.clear(); g_cfg.w = g_cfg.h = g_cfg.fps = 0; g_cfg.br = 0; }
        else { g_cfg.cam = s.cam; g_cfg.w = s.w; g_cfg.h = s.h; g_cfg.fps = s.fps; g_cfg.br = s.br; }
    }
    if (restart) g_restart = true;
    settings_save(s);
}

// ---------- phone camera list (from the `caps` reply) ----------
struct GMode { int w = 0, h = 0; std::vector<int> fps; std::string label, note; };
struct GCam { std::string id, name; std::vector<GMode> modes; };
struct GuiState {
    GuiSettings s;
    std::string capsTxt; std::vector<GCam> cams;
    int ci = 0, mi = 0, fi = 0;
    std::string defCam; int defW = 0, defH = 0, defFps = 0;
};

static std::string cam_name(const JV& e, bool lens, std::string facing) {
    if (!facing.empty()) facing[0] = (char)toupper((unsigned char)facing[0]);
    char b[96];
    if (lens) snprintf(b, sizeof b, "%s lens %.1f mm (id %s)", facing.c_str(), e.num("focal"), e.str("id").c_str());
    else snprintf(b, sizeof b, "%s camera (id %s)", facing.c_str(), e.str("id").c_str());
    return b;
}
static void parse_caps(GuiState& g) {
    g.cams.clear();
    JV root;
    if (g.capsTxt.empty() || !jparse_all(g.capsTxt, root)) return;
    const JV* base = root.get("caps") ? root.get("caps") : &root;
    const JV* cams = base->get("cameras");
    if (!cams) return;
    if (const JV* df = base->get("default")) { g.defCam = df->str("cam"); g.defW = (int)df->num("w"); g.defH = (int)df->num("h"); g.defFps = (int)df->num("fps"); }
    auto add = [&](const JV& e, bool lens, const std::string& facing) {
        const JV* m = e.get("modes");
        if (!m || m->a.empty()) return;
        GCam c; c.id = e.str("id"); c.name = cam_name(e, lens, facing);
        for (const JV& x : m->a) {
            GMode gm; gm.w = (int)x.num("w"); gm.h = (int)x.num("h"); gm.note = x.str("note");
            char lb[96], wh[32];
            snprintf(wh, sizeof wh, "%dx%d", gm.w, gm.h);
            std::string nm = x.str("label");
            if (nm.empty() || nm == wh) snprintf(lb, sizeof lb, "%s%s", wh, gm.note == "cropped" ? "  (zoomed in)" : gm.note == "unverified" ? "  (untested)" : "");
            else snprintf(lb, sizeof lb, "%s  %s%s", nm.c_str(), wh, gm.note == "cropped" ? "  (zoomed in)" : gm.note == "unverified" ? "  (untested)" : "");
            gm.label = lb;
            if (const JV* f = x.get("fps")) for (const JV& q : f->a) gm.fps.push_back((int)q.n);
            if (gm.fps.empty()) gm.fps.push_back(30);
            if (gm.w > 0 && gm.h > 0) c.modes.push_back(gm);
        }
        if (!c.modes.empty()) g.cams.push_back(c);
    };
    for (const JV& c : cams->a) {
        add(c, false, c.str("facing"));
        if (const JV* ph = c.get("phys")) for (const JV& p : ph->a) add(p, true, c.str("facing"));
    }
}
static void pick_mode(GuiState& g, int wantW, int wantH, int wantFps) {
    g.mi = 0; g.fi = 0;
    if (g.cams.empty()) return;
    GCam& c = g.cams[g.ci];
    bool found = false;
    for (size_t i = 0; i < c.modes.size() && !found; i++) if (c.modes[i].w == wantW && c.modes[i].h == wantH) { g.mi = (int)i; found = true; }
    for (size_t i = 0; i < c.modes.size() && !found; i++) if (c.modes[i].h == 1080) { g.mi = (int)i; found = true; }
    GMode& m = c.modes[g.mi];
    bool ff = false;
    for (size_t i = 0; i < m.fps.size() && !ff; i++) if (m.fps[i] == wantFps) { g.fi = (int)i; ff = true; }
    for (size_t i = 0; i < m.fps.size() && !ff; i++) if (m.fps[i] == 30) { g.fi = (int)i; ff = true; }
}
static void sync_selection(GuiState& g) {
    g.ci = 0;
    if (g.cams.empty()) return;
    bool found = false;
    for (size_t i = 0; i < g.cams.size() && !found; i++) if (!g.s.cam.empty() && g.cams[i].id == g.s.cam) { g.ci = (int)i; found = true; }
    for (size_t i = 0; i < g.cams.size() && !found; i++) if (g.cams[i].id == g.defCam) { g.ci = (int)i; found = true; }
    if (found && !g.s.cam.empty() && g.cams[g.ci].id == g.s.cam) pick_mode(g, g.s.w, g.s.h, g.s.fps);
    else pick_mode(g, g.defW, g.defH, g.defFps);
}
static void commit_selection(GuiState& g) {
    if (g.cams.empty()) return;
    GCam& c = g.cams[g.ci]; GMode& m = c.modes[g.mi];
    g.s.cam = c.id; g.s.w = m.w; g.s.h = m.h; g.s.fps = m.fps[g.fi];
}

// ---------- small widgets ----------
static bool Toggle(const char* id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight(), w = h * 1.8f, r = h * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool ch = false;
    if (ImGui::IsItemClicked()) { *v = !*v; ch = true; }
    ImU32 bg = *v ? IM_COL32(40, 170, 90, 255) : IM_COL32(95, 98, 112, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    dl->AddCircleFilled(ImVec2(*v ? p.x + w - r : p.x + r, p.y + r), r - 3.0f, IM_COL32(255, 255, 255, 255));
    return ch;
}
static void Section(const char* t, float sc) {
    ImGui::Dummy(ImVec2(0, 6 * sc));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.60f, 0.90f, 1));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 2 * sc));
}
template <class F> static auto imrender(F f, ImDrawData* d, SDL_Renderer* r, int) -> decltype(f(d, r), void()) { f(d, r); }
template <class F> static void imrender(F f, ImDrawData* d, SDL_Renderer*, long) { f(d); }

static void status_view(bool live, std::string& title, ImU32& col) {
    std::string s = get_status();
    const ImU32 green = IM_COL32(60, 200, 110, 255), amber = IM_COL32(235, 170, 50, 255), red = IM_COL32(230, 80, 80, 255), grey = IM_COL32(140, 143, 155, 255);
    if (live) { title = "Live"; col = green; }
    else if (s.rfind("waiting for phone", 0) == 0) { title = "Waiting for phone"; col = amber; }
    else if (s.rfind("cannot reach", 0) == 0) { title = "Can't reach the phone app"; col = red; }
    else if (s.rfind("phone camera is switched off", 0) == 0) { title = "Phone camera is off"; col = amber; }
    else if (s.rfind("idle", 0) == 0) { title = "Idle"; col = grey; }
    else if (s.rfind("connection lost", 0) == 0) { title = "Reconnecting"; col = amber; }
    else if (s.rfind("starting", 0) == 0 || s.rfind("streaming", 0) == 0 || s.rfind("restarting", 0) == 0) { title = "Connecting..."; col = amber; }
    else { title = s; col = amber; }
}

// ---------- the window ----------
static void gui_loop() {
    SetProcessDPIAware();
    UINT dpi = GetDpiForSystem();
    float sc = dpi ? (float)dpi / 96.0f : 1.0f;
    if (sc < 1.0f) sc = 1.0f;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { ulog("[error] SDL init failed: %s", SDL_GetError()); return; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* win = SDL_CreateWindow("UsbCam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1120 * sc), (int)(660 * sc), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!win || !ren) { ulog("[error] window failed: %s", SDL_GetError()); return; }
    SDL_SetWindowMinimumSize(win, (int)(760 * sc), (int)(560 * sc));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    char wd[MAX_PATH]; UINT wn = GetWindowsDirectoryA(wd, MAX_PATH);
    std::string ttf = std::string(wn ? wd : "C:\\Windows") + "\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(ttf.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(ttf.c_str(), 17.0f * sc);
    else io.FontGlobalScale = sc * 1.2f;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 6 * sc; st.GrabRounding = 6 * sc; st.PopupRounding = 6 * sc;
    st.ItemSpacing = ImVec2(8 * sc, 8 * sc); st.FramePadding = ImVec2(10 * sc, 6 * sc); st.WindowPadding = ImVec2(18 * sc, 16 * sc);
    st.WindowBorderSize = 0;
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.115f, 0.15f, 1);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.23f, 1);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.14f, 0.15f, 0.20f, 1);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.65f, 1.0f, 1);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.65f, 1.0f, 1);
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    GuiState gs;
    settings_load(gs.s);
    g_mirror = gs.s.mirror ? 1 : 0;
    gui_apply(gs.s, false);
    std::string vcam_msg;
    if (gs.s.vcam) {
        if (g_vcam_supported) { std::string e; if (!vcam_enable(true, e)) { vcam_msg = e; gs.s.vcam = false; } }
        else gs.s.vcam = false;
    }

    { std::lock_guard<std::mutex> l(g_prevmu); g_prev = av_frame_alloc(); }
    AVFrame* show = av_frame_alloc();
    SDL_Texture* tex = nullptr; int tw = 0, th = 0, tfmt = -1;
    bool have_frame = false, running = true;
    g_gui_active = true;

    while (running && g_run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
        }
        bool mini = (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) != 0;
        g_gui_active = !mini;
        if (mini) { SDL_Delay(150); continue; }

        bool newf = false;
        {
            std::lock_guard<std::mutex> l(g_prevmu);
            if (g_prev_new) { av_frame_unref(show); av_frame_move_ref(show, g_prev); g_prev_new = false; newf = true; have_frame = true; }
        }
        if (newf && show->width > 0) {
            int fmt = show->format;
            if (fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_YUV420P) {
                if (!tex || tw != show->width || th != show->height || tfmt != fmt) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, fmt == AV_PIX_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, show->width, show->height);
                    tw = show->width; th = show->height; tfmt = fmt;
                }
                if (tex) {
                    if (fmt == AV_PIX_FMT_NV12) SDL_UpdateNVTexture(tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1]);
                    else SDL_UpdateYUVTexture(tex, nullptr, show->data[0], show->linesize[0], show->data[1], show->linesize[1], show->data[2], show->linesize[2]);
                }
            }
        }

        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        float panelW = 390 * sc; if (panelW > ww * 0.5f) panelW = ww * 0.5f;
        bool live = have_frame && (now_us() - g_prev_us.load()) < 1500000;
        SDL_SetRenderDrawColor(ren, 12, 13, 17, 255);
        SDL_RenderClear(ren);
        if (tex) {
            float aw = ww - panelW - 32 * sc, ah = wh - 32 * sc;
            float s = std::min(aw / (float)tw, ah / (float)th);
            if (s > 0) {
                int dw = (int)(tw * s), dh = (int)(th * s);
                SDL_Rect dst = { (int)((ww - panelW - dw) / 2), (wh - dh) / 2, dw, dh };
                Uint8 c = live ? 255 : 80;
                SDL_SetTextureColorMod(tex, c, c, c);
                SDL_RenderCopyEx(ren, tex, nullptr, &dst, 0, nullptr, g_mirror.load() ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
            }
        }

        // phone info (battery etc.) and the code the phone is really sending
        std::string pst, actual;
        { std::lock_guard<std::mutex> l(g_infomu); pst = g_pstatus; actual = g_actual;
          if (g_caps != gs.capsTxt) { gs.capsTxt = g_caps; parse_caps(gs); sync_selection(gs); } }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        std::string title; ImU32 dot; status_view(live, title, dot);
        // big centred text on the video area while nothing is live
        if (!live) {
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            std::string sub = get_sub();
            float cx = (ww - panelW) * 0.5f, cy = wh * 0.5f;
            ImVec2 t1 = ImGui::CalcTextSize(title.c_str());
            bg->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f, ImVec2(cx - t1.x * 0.8f, cy - 30 * sc), IM_COL32(235, 238, 250, 255), title.c_str());
            ImVec2 t2 = ImGui::CalcTextSize(sub.c_str());
            bg->AddText(ImVec2(cx - t2.x * 0.5f, cy + 14 * sc), IM_COL32(150, 156, 180, 255), sub.c_str());
        }

        ImGui::SetNextWindowPos(ImVec2((float)ww - panelW, 0));
        ImGui::SetNextWindowSize(ImVec2(panelW, (float)wh));
        ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

        // status
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float r = 7 * sc;
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f + 2), r, dot);
            ImGui::Dummy(ImVec2(r * 2 + 6 * sc, 0)); ImGui::SameLine();
            ImGui::TextUnformatted(title.c_str());
            double fps = g_stat_fps.load(), lat = g_stat_lat.load();
            if (live && fps > 0) { ImGui::TextDisabled("%.0f fps  |  about %.0f ms from camera to PC", fps, lat); }
            else { std::string sub = get_sub(); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", sub.c_str()); ImGui::PopTextWrapPos(); }
        }

        // virtual camera
        Section("VIRTUAL CAMERA", sc);
        {
            bool v = g_vcam_on.load();
            ImGui::BeginDisabled(!g_vcam_supported);
            if (Toggle("##vc", &v)) {
                std::string err; bool ok = vcam_enable(v, err);
                vcam_msg = ok ? std::string() : err;
                gs.s.vcam = ok ? v : false;
                settings_save(gs.s);
            }
            ImGui::EndDisabled();
            ImGui::SameLine(); ImGui::TextUnformatted(g_vcam_on.load() ? "On - \"UsbCam Camera\"" : "Off");
            ImGui::PushTextWrapPos(0);
            if (!g_vcam_supported) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1)); ImGui::TextWrapped("%s", g_vcam_why.c_str()); ImGui::PopStyleColor(); }
            else if (!vcam_msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1)); ImGui::TextWrapped("%s", vcam_msg.c_str()); ImGui::PopStyleColor(); }
            else if (g_vcam_on.load()) {
                if (consumer_recent(2000)) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.85f, 0.55f, 1)); ImGui::TextWrapped("An app is using the camera."); ImGui::PopStyleColor(); }
                else ImGui::TextDisabled("Ready. Choose \"UsbCam Camera\" in Zoom, Meet, Teams or Discord.");
            }
            ImGui::PopTextWrapPos();
            bool mir = g_mirror.load() != 0;
            if (ImGui::Checkbox("Mirror picture", &mir)) {
                g_mirror = mir ? 1 : 0;
                if (g_sh.hdr) g_sh.hdr->mirror = g_mirror.load();
                gs.s.mirror = mir; settings_save(gs.s);
            }
        }

        // picture
        Section("PHONE PICTURE", sc);
        {
            bool pd = gs.s.phone_default;
            if (ImGui::Checkbox("Use the phone's own settings", &pd)) {
                gs.s.phone_default = pd;
                if (!pd) commit_selection(gs);
                gui_apply(gs.s, true);
            }
            if (!gs.s.phone_default) {
                if (gs.cams.empty()) {
                    ImGui::PushTextWrapPos(0); ImGui::TextDisabled("The camera list appears once the phone is connected."); ImGui::PopTextWrapPos();
                } else {
                    bool changed = false;
                    ImGui::TextDisabled("Camera");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##cam", gs.cams[gs.ci].name.c_str())) {
                        for (int i = 0; i < (int)gs.cams.size(); i++) {
                            if (ImGui::Selectable(gs.cams[i].name.c_str(), i == gs.ci)) { gs.ci = i; pick_mode(gs, 1920, 1080, 30); changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    GCam& c = gs.cams[gs.ci];
                    ImGui::TextDisabled("Resolution");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##res", c.modes[gs.mi].label.c_str())) {
                        for (int i = 0; i < (int)c.modes.size(); i++) {
                            if (ImGui::Selectable(c.modes[i].label.c_str(), i == gs.mi)) { int wf = c.modes[gs.mi].fps[gs.fi]; gs.mi = i; gs.fi = 0; GMode& m = c.modes[i]; for (size_t k = 0; k < m.fps.size(); k++) if (m.fps[k] == wf) gs.fi = (int)k; changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    GMode& m = c.modes[gs.mi];
                    char fb[32]; snprintf(fb, sizeof fb, "%d fps", m.fps[gs.fi]);
                    ImGui::TextDisabled("Frame rate");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##fps", fb)) {
                        for (int i = 0; i < (int)m.fps.size(); i++) {
                            char b2[32]; snprintf(b2, sizeof b2, "%d fps", m.fps[i]);
                            if (ImGui::Selectable(b2, i == gs.fi)) { gs.fi = i; changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    if (changed) { commit_selection(gs); gui_apply(gs.s, true); }
                }
                bool autoq = gs.s.br == 0;
                if (ImGui::Checkbox("Automatic quality", &autoq)) { gs.s.br = autoq ? 0 : 12000000; gui_apply(gs.s, true); }
                if (!autoq) {
                    int mb = gs.s.br / 1000000; if (mb < 2) mb = 2;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##br", &mb, 2, 60, "%d Mbit/s")) gs.s.br = mb * 1000000;
                    if (ImGui::IsItemDeactivatedAfterEdit()) gui_apply(gs.s, true);
                }
            }
            if (!actual.empty()) {
                JV av;
                if (jparse_all(actual, av) && av.t == 'o' && av.num("w") > 0)
                    ImGui::TextDisabled("Sending %dx%d, %d fps, %.1f Mbit/s", (int)av.num("w"), (int)av.num("h"), (int)av.num("fps"), av.num("br") / 1e6);
            }
        }

        // phone
        Section("PHONE", sc);
        {
            JV pv;
            if (!pst.empty() && jparse_all(pst, pv) && pv.t == 'o' && pv.get("batt")) {
                ImGui::Text("Battery %d%%%s", (int)pv.num("batt"), pv.num("plugged") > 0 ? "  (charging)" : "");
                ImGui::Text("Temperature %.1f C", pv.num("tempC"));
                if (pv.num("guardStage") > 0) {
                    std::string gm = pv.str("guard");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.30f, 1));
                    ImGui::PushTextWrapPos(0); ImGui::TextWrapped("%s", gm.empty() ? "The phone is protecting itself (heat or battery)." : gm.c_str()); ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            } else ImGui::TextDisabled("Not connected");
        }

        ImGui::Dummy(ImVec2(0, 10 * sc));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Minimise this window to keep the virtual camera running in the background. Closing it turns the camera off.");
        ImGui::PopTextWrapPos();
        ImGui::End();

        ImGui::Render();
        imrender(ImGui_ImplSDLRenderer2_RenderDrawData, ImGui::GetDrawData(), ren, 0);
        SDL_RenderPresent(ren);
    }

    g_gui_active = false;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (tex) SDL_DestroyTexture(tex);
    av_frame_free(&show);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
}
