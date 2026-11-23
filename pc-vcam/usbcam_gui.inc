// usbcam_gui.inc: the UsbCam window (live preview + controls). Included by usbcam_vcam.cpp when UCV_GUI is defined.
// Uses SDL2 for the window and Dear ImGui (MIT) for the controls. Original code.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <algorithm>
#include <cmath>

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
    bool vcam = true, mirror = false, phone_default = true, light = false, preview = true;
    int tsz = 0;   // text size level 0..3 = 100/125/150/200 %
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
        else if (k == "light") s.light = v == "1";
        else if (k == "preview") s.preview = v != "0";
        else if (k == "tsz") s.tsz = std::max(0, std::min(3, atoi(v.c_str())));
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
    fprintf(f, "vcam=%d\nmirror=%d\nphone_default=%d\nlight=%d\npreview=%d\ntsz=%d\ncam=%s\nw=%d\nh=%d\nfps=%d\nbr=%d\n",
            s.vcam ? 1 : 0, s.mirror ? 1 : 0, s.phone_default ? 1 : 0, s.light ? 1 : 0, s.preview ? 1 : 0, s.tsz, s.cam.c_str(), s.w, s.h, s.fps, s.br);
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

// ---------- Material Design 3 look: compact, high contrast (dark first, optional light) ----------
// Non-ASCII characters are written as byte escapes so the file does not depend on the compiler's source charset.
#define SEP " \xC2\xB7 "
#define MUL "\xC3\x97"
#define DEG "\xC2\xB0"
#include "usbcam_icon.h"

// Every text/background pair below is >= 4.5:1 and every control outline >= 3:1 (WCAG AA), in both themes.
struct Pal {
    ImVec4 surf, card, high, pop, stage, onSurf, onVar, outl, outV, pri, onPri, secC, onSecC;
    ImVec4 ok, okC, onOkC, warn, warnC, onWarnC, err, errC, onErrC;
};
static ImVec4 Hx(unsigned c) { return ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1.0f); }
static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f); }
static ImU32 Cu(const ImVec4& c) { return ImGui::GetColorU32(c); }   // also applies the disabled-state alpha

static Pal pal_dark() {
    Pal p;
    p.surf = Hx(0x111318); p.card = Hx(0x1C1E24); p.high = Hx(0x2B2E36); p.pop = Hx(0x262930); p.stage = Hx(0x0B0C10);
    p.onSurf = Hx(0xECEAF2); p.onVar = Hx(0xCFCEDA); p.outl = Hx(0x8E90A0); p.outV = Hx(0x363944);
    p.pri = Hx(0xAEBFFF); p.onPri = Hx(0x12236F); p.secC = Hx(0x3A3F5C); p.onSecC = Hx(0xE4E6FF);
    p.ok = Hx(0x7FE0A0); p.okC = Hx(0x0F4A2C); p.onOkC = Hx(0xB4F7CB);
    p.warn = Hx(0xFFC36B); p.warnC = Hx(0x57400B); p.onWarnC = Hx(0xFFE2B0);
    p.err = Hx(0xFFB4AB); p.errC = Hx(0x8C0A12); p.onErrC = Hx(0xFFE1DD);
    return p;
}
static Pal pal_light() {
    Pal p;
    p.surf = Hx(0xF6F5FB); p.card = Hx(0xFFFFFF); p.high = Hx(0xE7E7F0); p.pop = Hx(0xFFFFFF); p.stage = Hx(0xE2E3EC);
    p.onSurf = Hx(0x14151B); p.onVar = Hx(0x3B3D48); p.outl = Hx(0x6B6E7C); p.outV = Hx(0xD3D5E0);
    p.pri = Hx(0x3446B0); p.onPri = Hx(0xFFFFFF); p.secC = Hx(0xDDE2FF); p.onSecC = Hx(0x0F1640);
    p.ok = Hx(0x146B3A); p.okC = Hx(0xC3F2D3); p.onOkC = Hx(0x00210E);
    p.warn = Hx(0x7A4600); p.warnC = Hx(0xFFE2B8); p.onWarnC = Hx(0x2A1400);
    p.err = Hx(0xAF1414); p.errC = Hx(0xFFDAD6); p.onErrC = Hx(0x410002);
    return p;
}

static Pal g_pal;
static float g_sc = 1.0f;
static ImFont *f_body = nullptr, *f_small = nullptr, *f_label = nullptr, *f_title = nullptr, *f_head = nullptr;   // nullptr = default font
#define S(x) ((x) * g_sc)

static void apply_theme(const Pal& p) {
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = p.onSurf; c[ImGuiCol_TextDisabled] = p.onVar;
    c[ImGuiCol_WindowBg] = p.surf; c[ImGuiCol_ChildBg] = p.card; c[ImGuiCol_PopupBg] = p.pop;
    c[ImGuiCol_Border] = p.outl; c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.high; c[ImGuiCol_FrameBgHovered] = Mix(p.high, p.onSurf, 0.08f); c[ImGuiCol_FrameBgActive] = Mix(p.high, p.onSurf, 0.12f);
    c[ImGuiCol_Button] = p.secC; c[ImGuiCol_ButtonHovered] = Mix(p.secC, p.onSecC, 0.10f); c[ImGuiCol_ButtonActive] = Mix(p.secC, p.onSecC, 0.16f);
    c[ImGuiCol_Header] = p.secC; c[ImGuiCol_HeaderHovered] = Mix(p.pop, p.onSurf, 0.10f); c[ImGuiCol_HeaderActive] = Mix(p.pop, p.onSurf, 0.16f);
    c[ImGuiCol_SliderGrab] = p.pri; c[ImGuiCol_SliderGrabActive] = p.pri; c[ImGuiCol_CheckMark] = p.pri;
    c[ImGuiCol_Separator] = p.outV; c[ImGuiCol_SeparatorHovered] = p.outV; c[ImGuiCol_SeparatorActive] = p.outV;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_ScrollbarGrab] = p.outl;
    c[ImGuiCol_ScrollbarGrabHovered] = p.onVar; c[ImGuiCol_ScrollbarGrabActive] = p.onSurf;
    c[ImGuiCol_NavHighlight] = p.pri;
}

// ---------- text helpers ----------
static ImVec2 TSize(ImFont* f, const char* t, float wrap = 0.0f) {
    ImGui::PushFont(f); ImVec2 s = ImGui::CalcTextSize(t, nullptr, false, wrap); ImGui::PopFont(); return s;
}
static void DText(ImDrawList* dl, ImFont* f, ImVec2 pos, ImU32 col, const char* t, float wrap = 0.0f) {
    ImGui::PushFont(f); dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, col, t, nullptr, wrap); ImGui::PopFont();
}
static void TextC(ImFont* f, const ImVec4& c, const char* s, bool wrap = false) {
    ImGui::PushFont(f); ImGui::PushStyleColor(ImGuiCol_Text, c);
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor(); ImGui::PopFont();
}
static void SdlCol(SDL_Renderer* r, const ImVec4& c) {
    SDL_SetRenderDrawColor(r, (Uint8)(c.x * 255.0f + 0.5f), (Uint8)(c.y * 255.0f + 0.5f), (Uint8)(c.z * 255.0f + 0.5f), 255);
}
// keyboard focus ring (only while keyboard navigation is active)
static void FocusRing(ImDrawList* dl, ImVec2 a, ImVec2 b, float r) {
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) dl->AddRect(ImVec2(a.x - S(2), a.y - S(2)), ImVec2(b.x + S(2), b.y + S(2)), Cu(g_pal.pri), r + S(2), 0, S(2));
}

// ---------- M3 widgets (compact) ----------
// filled card: 12dp radius, 12dp padding, 1dp outline, grows with its content
static void CardBegin(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
}
static void CardEnd() {
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x + 0.5f, p.y + 0.5f), ImVec2(p.x + s.x - 0.5f, p.y + s.y - 0.5f), Cu(g_pal.outV), S(12), 0, 1.0f);
    ImGui::EndChild();
}

// M3 switch, compact: 44x24dp track; the thumb grows and shows a check when on
static bool Switch(const char* id, bool* v) {
    const float sc = g_sc, w = 48.0f * sc, h = 28.0f * sc;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool ch = false;
    if (ImGui::InvisibleButton(id, ImVec2(w, h))) { *v = !*v; ch = true; }
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float goal = *v ? 1.0f : 0.0f;
    float& tr = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), goal);
    tr += (goal - tr) * std::min(1.0f, ImGui::GetIO().DeltaTime * 20.0f);
    const float t = tr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 off = hov ? Mix(g_pal.high, g_pal.onSurf, 0.08f) : g_pal.high;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Cu(Mix(off, g_pal.pri, t)), h * 0.5f);
    if (t < 0.98f) {
        ImVec4 o = g_pal.outl; o.w = 1.0f - t;
        dl->AddRect(ImVec2(p.x + sc, p.y + sc), ImVec2(p.x + w - sc, p.y + h - sc), Cu(o), h * 0.5f, 0, 2.0f * sc);
    }
    float cx = p.x + 14.0f * sc + (w - 28.0f * sc) * t, cy = p.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, cy), (8.0f + 3.0f * t) * sc, Cu(Mix(g_pal.outl, g_pal.onPri, t)), 24);
    if (t > 0.5f) {
        ImVec4 k = g_pal.pri; k.w = (t - 0.5f) * 2.0f;
        ImVec2 pts[3] = { ImVec2(cx - 3.5f * sc, cy), ImVec2(cx - 1.0f * sc, cy + 2.5f * sc), ImVec2(cx + 3.5f * sc, cy - 2.5f * sc) };
        dl->AddPolyline(pts, 3, Cu(k), 0, 1.8f * sc);
    }
    FocusRing(dl, p, ImVec2(p.x + w, p.y + h), h * 0.5f);
    return ch;
}
// list row: label on the left, switch on the right
static bool SwitchRow(const char* id, const char* label, bool* v, bool title) {
    const float h = 28.0f * g_sc;
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* f = title ? f_title : f_body;
    ImGui::PushFont(f);
    float th = ImGui::GetTextLineHeight();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + (h - th) * 0.5f));
    ImGui::TextUnformatted(label);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(p.x + avail - 48.0f * g_sc, p.y));
    return Switch(id, v);
}
// M3 segmented button (32dp); returns the (possibly new) selected index
static int Segmented(const char* id, const char* const* lab, int n, int sel) {
    const float sc = g_sc, h = 36.0f * sc;
    float w = ImGui::GetContentRegionAvail().x, sw = w / (float)n;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int res = sel;
    ImGui::PushID(id);
    for (int i = 0; i < n; i++) {
        ImVec2 a(p.x + sw * (float)i, p.y), b(p.x + sw * (float)(i + 1), p.y + h);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool pr = ImGui::InvisibleButton("##seg", ImVec2(b.x - a.x, h));
        bool hov = ImGui::IsItemHovered();
        FocusRing(dl, a, b, h * 0.5f);
        ImGui::PopID();
        if (pr) res = i;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawFlags fl = n == 1 ? ImDrawFlags_RoundCornersAll : (i == 0 ? ImDrawFlags_RoundCornersLeft : (i == n - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone));
        bool on = i == sel;
        if (on) dl->AddRectFilled(a, b, Cu(g_pal.secC), h * 0.5f, fl);
        else if (hov) dl->AddRectFilled(a, b, Cu(Mix(g_pal.card, g_pal.onSurf, 0.08f)), h * 0.5f, fl);
        ImVec4 fg = on ? g_pal.onSecC : g_pal.onVar;
        ImVec2 ts = TSize(f_label, lab[i]);
        float ck = on ? 12.0f * sc + 6.0f * sc : 0.0f;
        float x0 = a.x + ((b.x - a.x) - (ts.x + ck)) * 0.5f, cy = a.y + h * 0.5f;
        if (on) {
            ImVec2 pts[3] = { ImVec2(x0, cy), ImVec2(x0 + 3.5f * sc, cy + 3.5f * sc), ImVec2(x0 + 10.0f * sc, cy - 3.5f * sc) };
            dl->AddPolyline(pts, 3, Cu(fg), 0, 2.0f * sc);
        }
        DText(dl, f_label, ImVec2(x0 + ck, cy - ts.y * 0.5f), Cu(fg), lab[i]);
    }
    ImGui::PopID();
    for (int i = 1; i < n; i++) dl->AddLine(ImVec2(p.x + sw * (float)i, p.y), ImVec2(p.x + sw * (float)i, p.y + h), Cu(g_pal.outl), 1.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), Cu(g_pal.outl), h * 0.5f, 0, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));
    ImGui::Dummy(ImVec2(w, h));
    return res;
}
// status chip (28dp pill with a dot and a label), drawn at a screen position, right edge at xr; returns nothing
static void ChipAt(ImDrawList* dl, float xr, float cy, const char* t, const ImVec4& bg, const ImVec4& fg, const ImVec4& dot) {
    const float sc = g_sc, h = 32.0f * sc, px = 12.0f * sc, r = 5.0f * sc;
    ImVec2 ts = TSize(f_label, t);
    float w = px + r * 2.0f + 8.0f * sc + ts.x + px;
    float x = xr - w, y = cy - h * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), Cu(bg), h * 0.5f);
    dl->AddCircleFilled(ImVec2(x + px + r, cy), r, Cu(dot), 16);
    DText(dl, f_label, ImVec2(x + px + r * 2.0f + 8.0f * sc, y + (h - ts.y) * 0.5f), Cu(fg), t);
}
static float ChipWidth(const char* t) { return 12.0f * g_sc * 2 + 9.0f * g_sc + 8.0f * g_sc + TSize(f_label, t).x; }
// label on the left, value on the right
static void KV(const char* k, const char* v) {
    ImVec2 vs = TSize(f_body, v);
    TextC(f_body, g_pal.onVar, k);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - vs.x);
    TextC(f_body, g_pal.onSurf, v);
}
// rounds the corners of a rectangle that was drawn with square corners: a thick stroke in the background colour just outside it
static void RoundOut(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float t, ImU32 col) {
    float h = t * 0.5f + 0.5f;
    dl->AddRect(ImVec2(a.x - h, a.y - h), ImVec2(b.x + h, b.y + h), col, r + h, ImDrawFlags_RoundCornersAll, t);
}
static void Overlay(ImDrawList* dl, ImVec2 pos, const char* t) {
    ImVec2 ts = TSize(f_small, t);
    float px = 10.0f * g_sc, h = ts.y + 10.0f * g_sc;
    dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + px * 2.0f, pos.y + h), IM_COL32(0, 0, 0, 185), h * 0.5f);
    DText(dl, f_small, ImVec2(pos.x + px, pos.y + 5.0f * g_sc), IM_COL32(255, 255, 255, 255), t);
}
// theme toggle glyph: sun (shown in dark theme) or moon (shown in light theme)
static void ThemeGlyph(ImDrawList* dl, ImVec2 c, bool toLight, ImU32 col, ImU32 bg) {
    const float sc = g_sc;
    if (toLight) {   // sun
        dl->AddCircleFilled(c, 4.5f * sc, col, 20);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f, ca = cosf(a), sa = sinf(a);
            dl->AddLine(ImVec2(c.x + ca * 7.0f * sc, c.y + sa * 7.0f * sc), ImVec2(c.x + ca * 9.5f * sc, c.y + sa * 9.5f * sc), col, 1.8f * sc);
        }
    } else {         // moon
        dl->AddCircleFilled(c, 7.5f * sc, col, 24);
        dl->AddCircleFilled(ImVec2(c.x + 4.0f * sc, c.y - 3.5f * sc), 6.5f * sc, bg, 24);
    }
}

template <class F> static auto imrender(F f, ImDrawData* d, SDL_Renderer* r, int) -> decltype(f(d, r), void()) { f(d, r); }
template <class F> static void imrender(F f, ImDrawData* d, SDL_Renderer*, long) { f(d); }

// 0 idle, 1 live, 2 waiting / working, 3 error
static int status_view(bool live, std::string& title) {
    std::string s = get_status();
    if (live) { title = "Live"; return 1; }
    if (s.rfind("waiting for phone", 0) == 0) { title = "Waiting for phone"; return 2; }
    if (s.rfind("cannot reach", 0) == 0) { title = "Can't reach the phone app"; return 3; }
    if (s.rfind("cannot open video", 0) == 0) { title = "Video connection failed"; return 3; }
    if (s.rfind("decoder open failed", 0) == 0) { title = "Decoder failed"; return 3; }
    if (s.rfind("phone refused", 0) == 0) { title = "Phone refused to start"; return 3; }
    if (s.rfind("phone camera is switched off", 0) == 0) { title = "Phone camera is off"; return 2; }
    if (s.rfind("idle", 0) == 0) { title = "Idle"; return 0; }
    if (s.rfind("connection lost", 0) == 0) { title = "Reconnecting"; return 2; }
    if (s.rfind("starting", 0) == 0 || s.rfind("streaming", 0) == 0 || s.rfind("restarting", 0) == 0) { title = "Connecting..."; return 2; }
    title = s.empty() ? std::string("Starting...") : (s.size() > 30 ? s.substr(0, 30) + "..." : s);
    return 2;
}

// ---------- the window ----------
// (re)builds fonts and style for a given total scale (display DPI x text-size level)
static void build_ui(float sc) {
    g_sc = sc;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.FontGlobalScale = 1.0f;
    f_body = f_small = f_label = f_title = f_head = nullptr;
    char wd[MAX_PATH]; UINT wn = GetWindowsDirectoryA(wd, MAX_PATH);
    std::string fdir = std::string(wn ? wd : "C:\\Windows") + "\\Fonts\\";
    std::string reg = fdir + "segoeui.ttf", semi = fdir + "seguisb.ttf";
    if (GetFileAttributesA(reg.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesA(semi.c_str()) == INVALID_FILE_ATTRIBUTES) semi = reg;
        ImFontConfig fc; fc.OversampleH = 2; fc.OversampleV = 2;   // smooth, even strokes
        f_body = io.Fonts->AddFontFromFileTTF(reg.c_str(), 18.0f * sc, &fc);     // body (default font)
        f_small = io.Fonts->AddFontFromFileTTF(reg.c_str(), 16.0f * sc, &fc);    // supporting text (never smaller than 16)
        f_label = io.Fonts->AddFontFromFileTTF(semi.c_str(), 17.0f * sc, &fc);   // buttons, chips, field labels
        f_title = io.Fonts->AddFontFromFileTTF(semi.c_str(), 19.0f * sc, &fc);   // card titles
        f_head = io.Fonts->AddFontFromFileTTF(semi.c_str(), 26.0f * sc, &fc);    // app name / empty-state headline
    } else io.FontGlobalScale = sc * 1.2f;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 0; st.ChildRounding = 12 * sc; st.FrameRounding = 8 * sc; st.GrabRounding = 8 * sc; st.PopupRounding = 8 * sc;
    st.ScrollbarRounding = 4 * sc; st.ScrollbarSize = 8 * sc; st.GrabMinSize = 16 * sc;
    st.ItemSpacing = ImVec2(8 * sc, 6 * sc); st.ItemInnerSpacing = ImVec2(6 * sc, 6 * sc);
    st.FramePadding = ImVec2(10 * sc, 7 * sc); st.WindowPadding = ImVec2(12 * sc, 12 * sc);
    st.WindowBorderSize = 0; st.ChildBorderSize = 0; st.PopupBorderSize = 1; st.FrameBorderSize = 1;
}

static void gui_loop() {
    SetProcessDPIAware();
    UINT dpi = GetDpiForSystem();
    float dsc = dpi ? (float)dpi / 96.0f : 1.0f;
    if (dsc < 1.0f) dsc = 1.0f;
    static const float tmul[4] = { 1.0f, 1.25f, 1.5f, 2.0f };   // text size levels (WCAG: readable up to 200 %)
    GuiState gs;
    settings_load(gs.s);
    int cur_tsz = gs.s.tsz;
    float sc = dsc * tmul[cur_tsz];
    g_sc = sc;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { ulog("[error] SDL init failed: %s", SDL_GetError()); return; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* win = SDL_CreateWindow("UsbCam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1120 * dsc * std::min(tmul[cur_tsz], 1.25f)), (int)(680 * dsc * std::min(tmul[cur_tsz], 1.25f)), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (win && !ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!win || !ren) { ulog("[error] window failed: %s", SDL_GetError()); return; }
    auto set_min = [&](float s2) {   // minimum window size, never larger than the screen
        SDL_Rect db; int mw = (int)(860 * s2), mh = (int)(540 * s2);
        if (SDL_GetDisplayBounds(0, &db) == 0) { mw = std::min(mw, db.w); mh = std::min(mh, db.h); }
        SDL_SetWindowMinimumSize(win, mw, mh);
        int cw = 0, ch = 0; SDL_GetWindowSize(win, &cw, &ch);
        if (cw < mw || ch < mh) SDL_SetWindowSize(win, std::max(cw, mw), std::max(ch, mh));
    };
    set_min(sc);
    // same icon as the phone app: window / taskbar icon and the logo in the header
    SDL_Surface* isf = SDL_CreateRGBSurfaceWithFormatFrom((void*)g_icon_rgba, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
    if (isf) { SDL_SetWindowIcon(win, isf); SDL_FreeSurface(isf); }
    SDL_Texture* iconTex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, 64, 64);
    if (iconTex) { SDL_UpdateTexture(iconTex, nullptr, g_icon_rgba, 64 * 4); SDL_SetTextureBlendMode(iconTex, SDL_BLENDMODE_BLEND); }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // Tab / arrows / Space work on every control
    g_pal = gs.s.light ? pal_light() : pal_dark();
    build_ui(sc);
    apply_theme(g_pal);
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
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
        const bool pv = gs.s.preview;
        if (!pv && tex) { SDL_DestroyTexture(tex); tex = nullptr; tw = th = 0; tfmt = -1; }   // preview off: free the picture
        if (pv && newf && show->width > 0) {
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

        // layout: 52dp app bar, preview stage on the left, 340dp control panel on the right, 28dp status bar, 12dp margins
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        float panelW = 380 * sc; if (panelW > ww * 0.5f) panelW = ww * 0.5f;
        const float mg = 12 * sc, pad = 8 * sc, barH = 56 * sc, statH = 32 * sc;
        const float sx0 = mg, sy0 = barH, sx1 = (float)ww - panelW, sy1 = (float)wh - statH;
        bool live = have_frame && (now_us() - g_prev_us.load()) < 1500000;

        SdlCol(ren, g_pal.surf);
        SDL_RenderClear(ren);
        SdlCol(ren, g_pal.stage);
        SDL_Rect stg = { (int)sx0, (int)sy0, (int)(sx1 - sx0), (int)(sy1 - sy0) };
        SDL_RenderFillRect(ren, &stg);
        bool vid = false;
        SDL_Rect dst = { 0, 0, 0, 0 };
        if (pv && tex && live) {
            float aw = sx1 - sx0 - 2 * pad, ah = sy1 - sy0 - 2 * pad;
            float s = std::min(aw / (float)tw, ah / (float)th);
            if (s > 0) {
                int dw = (int)(tw * s), dh = (int)(th * s);
                dst.x = (int)(sx0 + (sx1 - sx0 - dw) * 0.5f); dst.y = (int)(sy0 + (sy1 - sy0 - dh) * 0.5f); dst.w = dw; dst.h = dh;
                SDL_SetTextureColorMod(tex, 255, 255, 255);
                SDL_RenderCopyEx(ren, tex, nullptr, &dst, 0, nullptr, g_mirror.load() ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                vid = true;
            }
        }

        // phone info (battery etc.) and the code the phone is really sending
        std::string pst, actual;
        { std::lock_guard<std::mutex> l(g_infomu); pst = g_pstatus; actual = g_actual;
          if (g_caps != gs.capsTxt) { gs.capsTxt = g_caps; parse_caps(gs); sync_selection(gs); } }

        if (gs.s.tsz != cur_tsz) {   // text size changed: rebuild fonts and sizes before the frame starts
            cur_tsz = gs.s.tsz; sc = dsc * tmul[cur_tsz];
            build_ui(sc); apply_theme(g_pal);
            ImGui_ImplSDLRenderer2_DestroyDeviceObjects();
            set_min(sc);
            settings_save(gs.s);
            continue;
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        std::string title; int kind = status_view(live, title);
        double fps = g_stat_fps.load(), lat = g_stat_lat.load();
        ImDrawList* bg = ImGui::GetBackgroundDrawList();

        // preview stage: rounded corners for the stage and for the picture; state text when nothing is live
        RoundOut(bg, ImVec2(sx0, sy0), ImVec2(sx1, sy1), 16 * sc, 24 * sc, Cu(g_pal.surf));
        if (vid) {   // round the picture's corners; the stroke is clipped to the stage so it never paints outside it
            bg->PushClipRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1), true);
            RoundOut(bg, ImVec2((float)dst.x, (float)dst.y), ImVec2((float)(dst.x + dst.w), (float)(dst.y + dst.h)), 10 * sc, 14 * sc, Cu(g_pal.stage));
            bg->PopClipRect();
        }
        if (!live || !pv) {
            std::string sub = get_sub();
            std::string ttl = title;
            if (live) { ttl = "Preview is off"; sub = "The phone is connected and streaming, and the virtual camera still works. Turn on Live preview to see the picture here."; }
            float bw = std::min(sx1 - sx0 - 48 * sc, 440 * sc);
            ImVec2 t1 = TSize(f_head, ttl.c_str(), bw), t2 = TSize(f_body, sub.c_str(), bw);
            float isz = 72 * sc, gap = 14 * sc;
            float total = isz + gap + t1.y + 6 * sc + t2.y;
            float cx = (sx0 + sx1) * 0.5f, y = (sy0 + sy1) * 0.5f - total * 0.5f;
            if (iconTex) bg->AddImage((ImTextureID)iconTex, ImVec2(cx - isz * 0.5f, y), ImVec2(cx + isz * 0.5f, y + isz));
            y += isz + gap;
            DText(bg, f_head, ImVec2(cx - t1.x * 0.5f, y), Cu(g_pal.onSurf), ttl.c_str(), bw);
            y += t1.y + 6 * sc;
            DText(bg, f_body, ImVec2(cx - t2.x * 0.5f, y), Cu(g_pal.onVar), sub.c_str(), bw);
        } else if (vid) {
            char ov[96];
            snprintf(ov, sizeof ov, "%d" MUL "%d" SEP "%.0f fps" SEP "%.0f ms", tw, th, fps, lat);
            float oh = TSize(f_small, ov).y + 10 * sc;
            Overlay(bg, ImVec2((float)dst.x + 10 * sc, (float)(dst.y + dst.h) - 10 * sc - oh), ov);
        }

        // ----- app bar: logo, name, status chip, theme button -----
        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2((float)ww, barH));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("##bar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            float cy = barH * 0.5f, lx = 16 * sc, isz = 32 * sc;
            if (iconTex) dl->AddImage((ImTextureID)iconTex, ImVec2(lx, cy - isz * 0.5f), ImVec2(lx + isz, cy + isz * 0.5f));
            ImVec2 nt = TSize(f_head, "UsbCam");
            DText(dl, f_head, ImVec2(lx + isz + 10 * sc, cy - nt.y * 0.5f), Cu(g_pal.onSurf), "UsbCam");

            // round tonal buttons on the right: text smaller, text larger, theme
            const float bs = 40 * sc;
            auto tbtn = [&](const char* id, ImVec2 pp, bool en, const char* tip, const char* txt, bool glyph) -> bool {
                ImGui::SetCursorScreenPos(pp);
                bool pr = ImGui::InvisibleButton(id, ImVec2(bs, bs));
                bool hv = ImGui::IsItemHovered();
                if (hv) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::PushFont(f_small); ImGui::SetTooltip("%s", tip); ImGui::PopFont(); }
                ImVec4 fill = (hv && en) ? Mix(g_pal.secC, g_pal.onSecC, 0.10f) : g_pal.secC;
                ImVec2 ctr(pp.x + bs * 0.5f, pp.y + bs * 0.5f);
                dl->AddCircleFilled(ctr, bs * 0.5f, Cu(fill), 32);
                if (glyph) ThemeGlyph(dl, ctr, !gs.s.light, Cu(g_pal.onSecC), Cu(fill));
                else { ImVec2 ts = TSize(f_label, txt); DText(dl, f_label, ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f), Cu(en ? g_pal.onSecC : g_pal.outl), txt); }
                FocusRing(dl, pp, ImVec2(pp.x + bs, pp.y + bs), bs * 0.5f);
                return pr && en;
            };
            ImVec2 bp((float)ww - 16 * sc - bs, cy - bs * 0.5f);
            if (tbtn("##theme", bp, true, gs.s.light ? "Switch to dark theme" : "Switch to light theme", "", true)) {
                gs.s.light = !gs.s.light;
                g_pal = gs.s.light ? pal_light() : pal_dark();
                apply_theme(g_pal);
                settings_save(gs.s);
            }
            ImVec2 bpl(bp.x - bs - 8 * sc, bp.y), bmi(bpl.x - bs - 8 * sc, bp.y);
            if (tbtn("##tplus", bpl, gs.s.tsz < 3, "Larger text", "A+", false)) gs.s.tsz++;
            if (tbtn("##tminus", bmi, gs.s.tsz > 0, "Smaller text", "A-", false)) gs.s.tsz--;

            // status chip, left of the theme button
            ImVec4 cbg = g_pal.high, cfg = g_pal.onSurf, cdot = g_pal.outl;
            if (kind == 1) { cbg = g_pal.okC; cfg = g_pal.onOkC; cdot = g_pal.ok; cdot.w = 0.55f + 0.45f * (0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f)); }
            else if (kind == 2) { cbg = g_pal.warnC; cfg = g_pal.onWarnC; cdot = g_pal.warn; }
            else if (kind == 3) { cbg = g_pal.errC; cfg = g_pal.onErrC; cdot = g_pal.err; }
            float cw = ChipWidth(title.c_str());
            float room = bmi.x - 12 * sc - (lx + isz + 10 * sc + nt.x + 16 * sc);
            std::string ct = title;
            if (cw > room && ct.size() > 14) ct = ct.substr(0, 12) + "...";
            ChipAt(dl, bmi.x - 12 * sc, cy, ct.c_str(), cbg, cfg, cdot);
            ImGui::End();
        }

        // ----- control panel (right), scrolls if the window is short -----
        ImGui::SetNextWindowPos(ImVec2((float)ww - panelW, barH));
        ImGui::SetNextWindowSize(ImVec2(panelW, sy1 - barH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(mg, 0));
        ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();

        // virtual camera
        CardBegin("##vcam");
        {
            bool v = g_vcam_on.load();
            ImGui::BeginDisabled(!g_vcam_supported);
            if (SwitchRow("##vc", "Virtual camera", &v, true)) {
                std::string err; bool ok = vcam_enable(v, err);
                vcam_msg = ok ? std::string() : err;
                gs.s.vcam = ok ? v : false;
                settings_save(gs.s);
            }
            ImGui::EndDisabled();
            if (!g_vcam_supported) TextC(f_small, g_pal.err, g_vcam_why.c_str(), true);
            else if (!vcam_msg.empty()) TextC(f_small, g_pal.err, vcam_msg.c_str(), true);
            else if (g_vcam_on.load()) {
                if (consumer_recent(2000)) TextC(f_small, g_pal.ok, "An app is using \"UsbCam Camera\".", true);
                else TextC(f_small, g_pal.onVar, "On" SEP "pick \"UsbCam Camera\" in Zoom, Meet, Teams or Discord.", true);
            } else TextC(f_small, g_pal.onVar, "Off", true);
            ImGui::Separator();
            bool mir = g_mirror.load() != 0;
            if (SwitchRow("##mir", "Mirror picture", &mir, false)) {
                g_mirror = mir ? 1 : 0;
                if (g_sh.hdr) g_sh.hdr->mirror = g_mirror.load();
                gs.s.mirror = mir; settings_save(gs.s);
            }
            ImGui::Separator();
            bool pvv = gs.s.preview;
            if (SwitchRow("##pv", "Live preview", &pvv, false)) { gs.s.preview = pvv; settings_save(gs.s); }
            TextC(f_small, g_pal.onVar, pvv ? "Shows the phone picture here. Turn off to save PC resources." : "Off. The phone and the virtual camera keep working.", true);
        }
        CardEnd();

        // phone (battery) - above the tall picture card so it never scrolls out of view
        CardBegin("##phone");
        {
            TextC(f_title, g_pal.onSurf, "Phone");
            JV pv;
            if (!pst.empty() && jparse_all(pst, pv) && pv.t == 'o' && pv.get("batt")) {
                char b1[48], b2[48];
                snprintf(b1, sizeof b1, "%d%%%s", (int)pv.num("batt"), pv.num("plugged") > 0 ? SEP "charging" : "");
                snprintf(b2, sizeof b2, "%.1f " DEG "C", pv.num("tempC"));
                KV("Battery", b1);
                KV("Temperature", b2);
                if (pv.num("guardStage") > 0) {
                    std::string gm = pv.str("guard");
                    TextC(f_small, g_pal.warn, gm.empty() ? "The phone is protecting itself (heat or battery)." : gm.c_str(), true);
                }
            } else TextC(f_small, g_pal.onVar, "Not connected", true);
        }
        CardEnd();

        // phone picture
        CardBegin("##pic");
        {
            TextC(f_title, g_pal.onSurf, "Phone picture");
            static const char* const srcLab[2] = { "Phone default", "Custom" };
            int ssel = gs.s.phone_default ? 0 : 1;
            int sn = Segmented("##src", srcLab, 2, ssel);
            if (sn != ssel) {
                gs.s.phone_default = sn == 0;
                if (!gs.s.phone_default) commit_selection(gs);
                gui_apply(gs.s, true);
            }
            if (!gs.s.phone_default) {
                if (gs.cams.empty()) {
                    TextC(f_small, g_pal.onVar, "The camera list appears once the phone is connected.", true);
                } else {
                    bool changed = false;
                    TextC(f_label, g_pal.onVar, "Camera");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##cam", gs.cams[gs.ci].name.c_str())) {
                        for (int i = 0; i < (int)gs.cams.size(); i++) {
                            if (ImGui::Selectable(gs.cams[i].name.c_str(), i == gs.ci)) { gs.ci = i; pick_mode(gs, 1920, 1080, 30); changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    GCam& c = gs.cams[gs.ci];
                    TextC(f_label, g_pal.onVar, "Resolution");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##res", c.modes[gs.mi].label.c_str())) {
                        for (int i = 0; i < (int)c.modes.size(); i++) {
                            if (ImGui::Selectable(c.modes[i].label.c_str(), i == gs.mi)) { int wf = c.modes[gs.mi].fps[gs.fi]; gs.mi = i; gs.fi = 0; GMode& m = c.modes[i]; for (size_t k = 0; k < m.fps.size(); k++) if (m.fps[k] == wf) gs.fi = (int)k; changed = true; }
                        }
                        ImGui::EndCombo();
                    }
                    GMode& m = c.modes[gs.mi];
                    char fb[32]; snprintf(fb, sizeof fb, "%d fps", m.fps[gs.fi]);
                    TextC(f_label, g_pal.onVar, "Frame rate");
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
                TextC(f_label, g_pal.onVar, "Quality");
                static const char* const qLab[2] = { "Automatic", "Manual" };
                int qsel = gs.s.br == 0 ? 0 : 1;
                int qn = Segmented("##q", qLab, 2, qsel);
                if (qn != qsel) { gs.s.br = qn == 0 ? 0 : 12000000; gui_apply(gs.s, true); }
                if (gs.s.br != 0) {
                    int mb = gs.s.br / 1000000; if (mb < 2) mb = 2;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::SliderInt("##br", &mb, 2, 60, "%d Mbit/s")) gs.s.br = mb * 1000000;
                    bool done = ImGui::IsItemDeactivatedAfterEdit();
                    if (done) gui_apply(gs.s, true);
                }
            }
        }
        CardEnd();
        ImGui::Dummy(ImVec2(0, 4 * sc));
        ImGui::End();

        // ----- status bar: hint on the left, what the phone is really sending on the right -----
        {
            const char* hint = "Minimise to keep the camera running. Closing this window turns it off.";
            float ty = (float)wh - statH + (statH - TSize(f_small, hint).y) * 0.5f;
            std::string snd;
            if (!actual.empty()) {
                JV av;
                if (jparse_all(actual, av) && av.t == 'o' && av.num("w") > 0) {
                    char sb[96];
                    snprintf(sb, sizeof sb, "Sending %d" MUL "%d" SEP "%d fps" SEP "%.1f Mbit/s", (int)av.num("w"), (int)av.num("h"), (int)av.num("fps"), av.num("br") / 1e6);
                    snd = sb;
                }
            }
            float rw = snd.empty() ? 0.0f : TSize(f_small, snd.c_str()).x;
            float hw = TSize(f_small, hint).x;
            if (!snd.empty()) DText(bg, f_small, ImVec2((float)ww - mg - rw, ty), Cu(g_pal.onVar), snd.c_str());
            if (mg + hw + 16 * sc < (float)ww - mg - rw) DText(bg, f_small, ImVec2(mg, ty), Cu(g_pal.onVar), hint);
        }

        ImGui::Render();
        imrender(ImGui_ImplSDLRenderer2_RenderDrawData, ImGui::GetDrawData(), ren, 0);
        SDL_RenderPresent(ren);
    }

    g_gui_active = false;
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (tex) SDL_DestroyTexture(tex);
    if (iconTex) SDL_DestroyTexture(iconTex);
    av_frame_free(&show);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
}
