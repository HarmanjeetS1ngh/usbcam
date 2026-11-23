// usbcam_update.inc: in-app update from GitHub Releases, the desktop twin of the phone app's Updater.kt.
// The Actions workflow (build-vcam.yml) publishes every desktop build as release "desktop-<run number>" with UsbCam-desktop.zip and
// RELEASE_NOTES_DESKTOP.md as the release text. The run number is compiled in as UCV_BUILD (usbcam_ver.h), so "newer" = a higher number.
// Included by usbcam_gui.inc (needs JV, ulog). The first part (release picking, notes text) has no Windows calls; the second part
// (WinHTTP, SHA-256, hand-over to the installer script) is Windows only. All network work runs on its own thread; the window only
// reads g_up.
#include "usbcam_ver.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#define UP_REPO "HarmanjeetS1ngh/usbcam"
#define UP_PREFIX "desktop-"

struct UpInfo { long long build = 0; std::string name, notes, url, digest; long long size = 0; };

// newest release tagged desktop-<n> that has a .zip asset on github.com/<repo>/releases/download/ (drafts and pre-releases are skipped)
static bool up_pick(const JV& arr, UpInfo& best) {
    if (arr.t != 'a') return false;
    bool found = false;
    const size_t plen = strlen(UP_PREFIX);
    const std::string base = std::string("https://github.com/") + UP_REPO + "/releases/download/";
    for (const JV& r : arr.a) {
        if (r.t != 'o' || r.num("draft") != 0 || r.num("prerelease") != 0) continue;
        std::string tag = r.str("tag_name");
        if (tag.compare(0, plen, UP_PREFIX) != 0) continue;
        char* e = nullptr;
        long long n = strtoll(tag.c_str() + plen, &e, 10);
        if (e == tag.c_str() + plen || *e != 0 || n <= 0) continue;
        const JV* as = r.get("assets");
        if (!as || as->t != 'a') continue;
        for (const JV& a : as->a) {
            std::string nm = a.str("name"), url = a.str("browser_download_url");
            if (nm.size() < 5 || nm.compare(nm.size() - 4, 4, ".zip") != 0) continue;
            if (url.compare(0, base.size(), base) != 0) continue;   // only ever download from this repo's releases
            if (!found || n > best.build) {
                best.build = n; best.name = r.str("name"); best.notes = r.str("body");
                best.url = url; best.size = (long long)a.num("size"); best.digest = a.str("digest");
                found = true;
            }
            break;
        }
    }
    return found;
}

// release text for the dialog: headings and ** dropped, one paragraph per item. The window font only has Latin-1, so typographic
// punctuation is mapped to ASCII and emoji are dropped (list items keep a plain "- ").
static std::string up_plain_notes(const std::string& md) {
    std::string t;
    for (size_t i = 0; i < md.size();) {
        unsigned char c = (unsigned char)md[i];
        if (c < 0x80) { if (c != '\r') t += (char)c; i++; continue; }
        unsigned cp = 0; int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n == 1 || i + (size_t)n > md.size()) { i++; continue; }
        cp = c & (0xFFu >> (n + 1));
        for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)md[i + (size_t)k] & 0x3F);
        i += (size_t)n;
        if (cp == 0x2019 || cp == 0x2018) t += '\'';
        else if (cp == 0x201C || cp == 0x201D) t += '"';
        else if (cp == 0x2013 || cp == 0x2014) t += '-';
        else if (cp == 0x2026) t += "...";
        else if (cp == 0x2022) t += "- ";
        else if (cp >= 0xA0 && cp <= 0xFF) { t += (char)(0xC0 | (cp >> 6)); t += (char)(0x80 | (cp & 0x3F)); }   // Latin-1: keep
        // everything else (emoji, variation selectors, ...) is dropped
    }
    std::string out, bold = "**";
    for (size_t p; (p = t.find(bold)) != std::string::npos;) t.erase(p, 2);
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t nl = t.find('\n', pos);
        std::string ln = t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? t.size() + 1 : nl + 1;
        size_t a = ln.find_first_not_of(" \t"), b = ln.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        ln = ln.substr(a, b - a + 1);
        if (ln[0] == '#') continue;
        if (ln.compare(0, 2, "- ") == 0 || ln.compare(0, 2, "* ") == 0) {
            size_t k = ln.find_first_not_of(" \t", 2);
            ln = k == std::string::npos ? std::string() : "- " + ln.substr(k);
            if (ln.empty()) continue;
        }
        if (!out.empty()) out += "\n\n";
        out += ln;
    }
    return out;
}

enum { UP_IDLE = 0, UP_CHECKING, UP_AVAIL, UP_DOWNLOADING, UP_APPLYING, UP_FAILED };
struct UpState {
    std::atomic<int> st{UP_IDLE};
    std::atomic<int> pct{-1};                              // download progress 0..100, -1 = the download has not started yet
    std::atomic<bool> cancel{false}, open_req{false}, quit{false};
    std::atomic<long long> dismissed{0};                   // build the user already said "Later" to (automatic checks stay quiet about it)
    std::mutex mu; UpInfo info; std::string err, toast;
};
static UpState g_up;

static void up_toast(const std::string& s) { std::lock_guard<std::mutex> l(g_up.mu); g_up.toast = s; }
static bool up_take_toast(std::string& s) { std::lock_guard<std::mutex> l(g_up.mu); if (g_up.toast.empty()) return false; s = g_up.toast; g_up.toast.clear(); return true; }
static std::string up_version_text() { char b[64]; snprintf(b, sizeof b, "%s (build %d)", UCV_VERSION, (int)UCV_BUILD); return b; }
static std::string up_size_text(long long n) { char b[32]; if (n >= 1048576) snprintf(b, sizeof b, "%.0f MB", (double)n / 1048576.0); else snprintf(b, sizeof b, "%lld KB", n / 1024); return b; }

#ifdef _WIN32
#include <winhttp.h>
#include <bcrypt.h>

struct UpHandle {
    HINTERNET h = nullptr;
    UpHandle() {}
    explicit UpHandle(HINTERNET x) : h(x) {}
    ~UpHandle() { if (h) WinHttpCloseHandle(h); }
    UpHandle(const UpHandle&) = delete; UpHandle& operator=(const UpHandle&) = delete;
};
static std::wstring up_w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string up_winerr(DWORD e) {
    if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT || e == ERROR_WINHTTP_CONNECTION_ERROR) return "no internet connection";
    if (e == ERROR_WINHTTP_TIMEOUT) return "GitHub did not answer in time";
    char b[48]; snprintf(b, sizeof b, "network error %lu", (unsigned long)e); return b;
}

// HTTPS GET (follows redirects). Every chunk goes to sink(data, size); returning false stops the transfer. [total] receives Content-Length
// (0 if unknown) before the first chunk. Returns "" on success, otherwise a short reason.
static std::string up_http_get(const std::string& url, bool json, const std::function<bool(const char*, size_t)>& sink, long long* total) {
    std::wstring wu = up_w(url);
    URL_COMPONENTSW uc = {}; uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) return "bad address";
    std::wstring target = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    UpHandle ses(WinHttpOpen(L"UsbCam-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) return up_winerr(GetLastError());
    WinHttpSetTimeouts(ses.h, 10000, 10000, 15000, 20000);
    UpHandle con(WinHttpConnect(ses.h, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0));
    if (!con.h) return up_winerr(GetLastError());
    UpHandle req(WinHttpOpenRequest(con.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return up_winerr(GetLastError());
    const wchar_t* hdr = json ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" : L"Accept: application/octet-stream\r\n";
    WinHttpAddRequestHeaders(req.h, hdr, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr))
        return up_winerr(GetLastError());
    DWORD code = 0, sz = sizeof code;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (code != 200) {
        if (code == 403 || code == 429) return "GitHub is limiting requests right now, try again later";
        char b[32]; snprintf(b, sizeof b, "GitHub answered HTTP %lu", (unsigned long)code); return b;
    }
    if (total) {
        DWORD cl = 0; sz = sizeof cl;
        *total = WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &cl, &sz, WINHTTP_NO_HEADER_INDEX) ? (long long)cl : 0;
    }
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD n = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &n)) return up_winerr(GetLastError());
        if (n == 0) return std::string();
        if (!sink(buf.data(), n)) return "stopped";
    }
}

struct UpSha {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; bool ok = false;
    UpSha() { ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0; }
    ~UpSha() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void add(const void* d, size_t n) { if (ok) BCryptHashData(h, (PUCHAR)d, (ULONG)n, 0); }
    std::string hex() {
        unsigned char o[32];
        if (!ok || BCryptFinishHash(h, o, 32, 0) < 0) return std::string();
        char b[65]; for (int i = 0; i < 32; i++) snprintf(b + i * 2, 3, "%02x", o[i]);
        return b;
    }
};

// ---- check ----
static void up_check_async(bool manual) {
    int st = g_up.st.load();
    if (manual && st == UP_AVAIL) { g_up.open_req = true; return; }   // already know about one: just show it again
    int exp = UP_IDLE;
    if (!g_up.st.compare_exchange_strong(exp, UP_CHECKING)) return;   // busy
    std::thread([manual] {
        std::string body;
        std::string err = up_http_get(std::string("https://api.github.com/repos/") + UP_REPO + "/releases?per_page=30", true,
                                      [&](const char* d, size_t n) { body.append(d, n); return body.size() < 4u * 1024 * 1024; }, nullptr);
        JV j; UpInfo best;
        if (err.empty() && !jparse_all(body, j)) err = "unexpected answer from GitHub";
        if (!err.empty()) {
            ulog("[update] check failed: %s", err.c_str());
            if (manual) up_toast("Couldn't check for updates: " + err);
            g_up.st = UP_IDLE;
            return;
        }
        if (up_pick(j, best) && best.build > (long long)UCV_BUILD) {
            ulog("[update] build %lld is available (installed %d)", best.build, (int)UCV_BUILD);
            { std::lock_guard<std::mutex> l(g_up.mu); g_up.info = best; g_up.err.clear(); }
            g_up.st = UP_AVAIL;
            if (manual || best.build != g_up.dismissed.load()) g_up.open_req = true;
        } else {
            ulog("[update] up to date (installed %d)", (int)UCV_BUILD);
            if (manual) up_toast("You're up to date" SEP + up_version_text());
            g_up.st = UP_IDLE;
        }
    }).detach();
}

// ---- install: the running exe cannot overwrite itself, so a small hidden PowerShell script waits for this process to exit,
// unpacks the zip next to it, copies the files over the install folder and starts UsbCam again. It leaves a one-line result
// in %APPDATA%\UsbCam\update-result.txt, which the next start shows as a message. The log is %TEMP%\usbcam-update.log. ----
static const char* kUpScript =
    "param([int]$ProcId, [string]$Zip, [string]$Dest, [string]$Exe)\r\n"
    "$ErrorActionPreference = 'Stop'\r\n"
    "$log = Join-Path $env:TEMP 'usbcam-update.log'\r\n"
    "$res = Join-Path $env:APPDATA 'UsbCam\\update-result.txt'\r\n"
    "function Log($m) { Add-Content -Path $log -Value ((Get-Date -Format s) + ' ' + $m) }\r\n"
    "try {\r\n"
    "  Log \"update start: $Zip -> $Dest\"\r\n"
    "  $stage = Join-Path $env:TEMP 'UsbCam-update\\stage'\r\n"
    "  if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }\r\n"
    "  Expand-Archive -LiteralPath $Zip -DestinationPath $stage -Force\r\n"
    "  if (-not (Test-Path (Join-Path $stage 'UsbCam.exe'))) { throw 'UsbCam.exe is missing in the update package' }\r\n"
    "  try { Wait-Process -Id $ProcId -Timeout 30 -ErrorAction SilentlyContinue } catch { }\r\n"
    "  Get-Process usbcam_vcam -ErrorAction SilentlyContinue | Stop-Process -Force\r\n"
    "  Start-Sleep -Milliseconds 500\r\n"
    "  $ok = $false\r\n"
    "  for ($i = 0; $i -lt 20 -and -not $ok; $i++) {\r\n"
    "    try { Copy-Item -Path (Join-Path $stage '*') -Destination $Dest -Recurse -Force; $ok = $true }\r\n"
    "    catch { Log ('copy attempt ' + $i + ' failed: ' + $_.Exception.Message); Start-Sleep -Milliseconds 500 }\r\n"
    "  }\r\n"
    "  if (-not $ok) { throw 'the files could not be replaced (in use, or the folder is read-only)' }\r\n"
    "  Log 'update done'\r\n"
    "  Set-Content -Path $res -Value 'ok'\r\n"
    "} catch {\r\n"
    "  Log ('update FAILED: ' + $_.Exception.Message)\r\n"
    "  Set-Content -Path $res -Value ('fail ' + $_.Exception.Message)\r\n"
    "}\r\n"
    "Start-Process -FilePath $Exe -WorkingDirectory $Dest\r\n";

static std::wstring up_temp_dir() {
    wchar_t b[MAX_PATH + 2]; DWORD n = GetTempPathW(MAX_PATH, b);
    std::wstring d(b, n);
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    d += L"UsbCam-update\\";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
static bool up_launch_apply(const std::wstring& dir, const std::wstring& zip, std::string& err) {
    std::wstring ps1 = dir + L"apply.ps1";
    FILE* f = _wfopen(ps1.c_str(), L"wb");
    if (!f) { err = "could not write the update script"; return false; }
    fwrite(kUpScript, 1, strlen(kUpScript), f); fclose(f);
    wchar_t exe[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { err = "could not find the program folder"; return false; }
    std::wstring ex(exe, n), dest = ex.substr(0, ex.find_last_of(L'\\'));   // no trailing backslash: it would escape the closing quote
    wchar_t sysd[MAX_PATH]; UINT sn = GetSystemDirectoryW(sysd, MAX_PATH);
    std::wstring psx = std::wstring(sysd, sn) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring cmd = L"\"" + psx + L"\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ps1 + L"\" -ProcId " +
                       std::to_wstring((unsigned long)GetCurrentProcessId()) + L" -Zip \"" + zip + L"\" -Dest \"" + dest + L"\" -Exe \"" + ex + L"\"";
    std::vector<wchar_t> cb(cmd.begin(), cmd.end()); cb.push_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cb.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        err = "could not start the update script"; return false;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

static void up_download_async() {
    int exp = UP_AVAIL;
    if (!g_up.st.compare_exchange_strong(exp, UP_DOWNLOADING)) { exp = UP_FAILED; if (!g_up.st.compare_exchange_strong(exp, UP_DOWNLOADING)) return; }
    g_up.cancel = false; g_up.pct = -1;
    std::thread([] {
        UpInfo info; { std::lock_guard<std::mutex> l(g_up.mu); info = g_up.info; g_up.err.clear(); }
        auto fail = [&](const std::string& e) { ulog("[update] %s", e.c_str()); { std::lock_guard<std::mutex> l(g_up.mu); g_up.err = e; } g_up.st = UP_FAILED; };
        std::wstring dir = up_temp_dir(), zip = dir + L"UsbCam-desktop.zip";
        DeleteFileW(zip.c_str());
        FILE* f = _wfopen(zip.c_str(), L"wb");
        if (!f) { fail("could not write to the temporary folder"); return; }
        UpSha sha; long long done = 0, total = 0;
        std::string err = up_http_get(info.url, false, [&](const char* d, size_t n) {
            if (g_up.cancel) return false;
            if (fwrite(d, 1, n, f) != n) return false;
            sha.add(d, n); done += n;
            long long t = info.size > 0 ? info.size : total;
            if (t > 0) g_up.pct = (int)std::min<long long>(100, done * 100 / t); else g_up.pct = 0;
            return true;
        }, &total);
        fclose(f);
        if (g_up.cancel) { DeleteFileW(zip.c_str()); g_up.st = UP_AVAIL; return; }
        if (!err.empty()) { DeleteFileW(zip.c_str()); fail(err == "stopped" ? std::string("could not save the download") : err); return; }
        if (info.size > 0 && done != info.size) { DeleteFileW(zip.c_str()); fail("the download was incomplete"); return; }
        if (info.digest.compare(0, 7, "sha256:") == 0) {   // GitHub computes this for every release asset
            std::string want = info.digest.substr(7), got = sha.hex();
            for (auto& c : want) c = (char)tolower((unsigned char)c);
            if (got.empty() || got != want) { DeleteFileW(zip.c_str()); fail("the download did not match GitHub's checksum"); return; }
        }
        std::string e2;
        if (!up_launch_apply(dir, zip, e2)) { fail(e2); return; }
        ulog("[update] build %lld downloaded, restarting to install it", info.build);
        g_up.pct = 100; g_up.st = UP_APPLYING; g_up.quit = true;
    }).detach();
}

// result of the last update (written by the script), shown once as a message after the restart
static std::string up_read_result() {
    const char* a = getenv("APPDATA"); if (!a) return std::string();
    std::string p = std::string(a) + "\\UsbCam\\update-result.txt";
    FILE* f = fopen(p.c_str(), "r"); if (!f) return std::string();
    char b[400] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    remove(p.c_str());
    std::string s = b;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s == "ok") return "Updated to " + up_version_text();
    if (s.compare(0, 4, "fail") == 0) return "The update did not finish" SEP "see %TEMP%\\usbcam-update.log";
    return std::string();
}
#else   // non-Windows builds (offline render/tests of the window): the network part does nothing
static void up_check_async(bool) {}
static void up_download_async() {}
static std::string up_read_result() { return std::string(); }
#endif
