// Tiro - Windows dictation app. Layer 1: tray shell, state machine, menu, dialogs.
// Spec: requirements_and_bdd.md (version 1.1.0).

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlobj.h>            // startup-folder shortcut (per-user, no scripts, no registry Run key)
#include <shobjidl.h>          // IShellLinkW
#include <objbase.h>           // CoInitializeEx for the shortcut writer
#include <uiautomation.h>      // UI Text Pattern: instant span selection for the polish rewrite
#include <richedit.h>          // CHARRANGE, EM_EXGETSEL/EM_EXSETSEL for the same

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <string>
#include <vector>
#include <cstdio>

#include "resource.h"
#include "punctuation.h"


#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "uiautomationcore.lib")

static const wchar_t* kAppName    = L"Tiro";
static const wchar_t* kAppVersion = L"1.1.0";
// A build's true identity is the commit it was compiled from. build_release.bat stamps it
// into src/gitver.h (generated, ignored) before every build; dirty trees get a -dirty tail.
// Rebuilding from the same commit always yields the same string - which a build counter never could.
#if __has_include("gitver.h")
#include "gitver.h"
#else
#define TIRO_GIT_HASH L"unknown"
#endif
static const wchar_t* kWndClass   = L"TiroMessageWindow";
static const wchar_t* kRegKey     = L"Software\\Tiro";

// Support/diagnostic switches: --about and --hotkey open a dialog directly.
#define WM_APP_TEST_ABOUT  (WM_APP + 3)
#define WM_APP_TEST_HOTKEY (WM_APP + 4)
#define WM_APP_TEST_NOTIFY (WM_APP + 7)
#define WM_APP_TEST_STARTUP (WM_APP + 14)   // --startup-toggle: run StartupToggle() directly
#define WM_APP_TEST_RESTART (WM_APP + 15)   // --restart: exercise the restart handoff

enum State { kLoading = 0, kOff = 1, kOn = 2 };

// Set once from the command line. A diagnostic COMMAND (--about, --notify, ...)
// must end the process when its job is done, or it lingers as a second tray
// icon; a bare --debug run is allowed to stay as a diagnostic instance.
static bool g_debug = false;

struct Device { std::wstring id, name; };
struct Model  { std::wstring folder, name; };

struct App {
    HINSTANCE hInst = nullptr;
    HWND      hwnd  = nullptr;
    HICON     icon[3] = { nullptr, nullptr, nullptr };
    int       state = kLoading;
    bool      trayAdded = false;
    UINT      wmTaskbarCreated = 0;
    UINT      hkMods = MOD_CONTROL | MOD_ALT;
    UINT      hkVk   = VK_SPACE;
    std::wstring deviceId;    // empty = follow system default
    std::wstring modelFolder; // empty = first valid model
    HWND      target  = nullptr;   // pinned when dictation turns on
    bool      targetFocused = false;   // has the pinned window had focus at least once?
    ULONGLONG onAt = 0;                // when dictation turned on
    ULONGLONG autoStopAt = 0;          // when the guard last stopped dictation
    HWND      lastApp = nullptr;   // last non-shell foreground window
    int pcores = 0, allcores = 0;
    std::vector<Device> devices;
    std::vector<Model>  models;
};
static App g;

// ---------------------------------------------------------------- paths / log

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? s : s.substr(0, pos);
}

// True when the command line carries `name` as a whole argument. Used for --debug, which
// gates every diagnostic switch (see the Diagnostics section of README.txt).
static bool HasSwitch(const wchar_t* name)
{
    int n = 0;
    LPWSTR* a = CommandLineToArgvW(GetCommandLineW(), &n);
    bool found = false;
    for (int i = 1; a && i < n; ++i)
        if (!_wcsicmp(a[i], name)) { found = true; break; }
    if (a) LocalFree(a);
    return found;
}

static std::wstring LocalAppDir()
{
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring base(buf);
    std::wstring dir = base + L"\\Tiro";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring LogPath() { return LocalAppDir() + L"\\app.log"; }

// The log is diagnostic, not an archive: keep the file small enough to read and
// mail. Rotated once at startup, so a session is never split mid-run.
static void RotateLogIfLarge()
{
    std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return;
    ULARGE_INTEGER sz{};
    sz.LowPart = fa.nFileSizeLow;
    sz.HighPart = fa.nFileSizeHigh;
    if (sz.QuadPart < 2u * 1024u * 1024u) return;   // 2 MB
    std::wstring old = path + L".old";
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// A log line is bounded on purpose. The only unbounded callers are the LogText
// diagnostics (a whole session's words, a whole rewrite), and swprintf_s aborts the
// process when a formatted line does not fit - the CRT invalid-parameter handler runs,
// not a truncation. A long dictation must never be able to kill the app it is being
// logged from, so the message is cut here instead.
static const size_t kLogMaxMsg = 1100;

static void Log(const wchar_t* level, const std::wstring& msg)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t head[80];
    swprintf_s(head, L"%04d-%02d-%02d %02d:%02d:%02d [%s] ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, level);
    std::wstring line = head;
    if (msg.size() > kLogMaxMsg) {
        line.append(msg, 0, kLogMaxMsg);
        line += L"... [truncated]";
    } else {
        line += msg;
    }
    line += L'\n';
    FILE* f = nullptr;
    if (_wfopen_s(&f, LogPath().c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fputws(line.c_str(), f);
        fclose(f);
    }
}

// ---------------------------------------------------------------- registry

static std::wstring RegGetStr(const wchar_t* name)
{
    HKEY k{};
    std::wstring out;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[512]{};
        DWORD cb = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ)
            out = buf;
        RegCloseKey(k);
    }
    return out;
}

static void RegSetStr(const wchar_t* name, const std::wstring& v)
{
    HKEY k{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }
}

static DWORD RegGetDword(const wchar_t* name, DWORD def)
{
    HKEY k{};
    DWORD out = def, cb = sizeof(DWORD), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&out, &cb);
        RegCloseKey(k);
    }
    return out;
}

// Diagnosed-log switch, off by default: with HKCU\Software\Tiro\LogText = 1 the log
// carries the dictated words (inject/utterance lines), which is how a session is
// debugged against its capture WAV. Default off so app.log never contains speech -
// that is the promise README.txt and SECURITY.md make. Read once per process.
static bool LogTextEnabled()
{
    static const bool on = RegGetDword(L"LogText", 0) != 0;
    return on;
}

static void RegSetDword(const wchar_t* name, DWORD v)
{
    HKEY k{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------------- cpu topology

// P-cores are the cores with the highest EfficiencyClass; all-zero means a homogeneous CPU,
// in which case every physical core is used (spec 3).
static void DetectCores()
{
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return;
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) return;

    std::vector<BYTE> classes;
    DWORD off = 0;
    while (off < len) {
        auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) classes.push_back(e->Processor.EfficiencyClass);
        if (!e->Size) break;
        off += e->Size;
    }
    g.allcores = (int)classes.size();
    if (classes.empty()) { g.pcores = 0; return; }
    BYTE top = *std::max_element(classes.begin(), classes.end());
    g.pcores = (int)std::count(classes.begin(), classes.end(), top);
}

// Windows 11 parks the threads of a tray-started process on the E-core group, where this model
// decodes several times slower - the same failure was measured and fixed on the sidecar STT
// server, so it is fixed the same way: restrict the process to the logical processors of the
// highest EfficiencyClass before the recogniser spawns its threads. Returns a line for the log;
// the caller logs it, because logging is not available this early.
static std::wstring PinToPcores()
{
    if (RegGetDword(L"PinCores", 1) == 0) return L"core pinning disabled by registry";
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (!len) return L"core pinning: no processor information";
    std::vector<BYTE> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
        return L"core pinning: query failed";

    BYTE top = 0; bool hybrid = false, multiGroup = false, zeroMask = false; int picked = 0;
    KAFFINITY mask = 0;
    DWORD off = 0;
    while (off < len) {
        auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) {
            BYTE eff = e->Processor.EfficiencyClass;
            if (eff > top) top = eff;
            if (e->Processor.GroupMask[0].Group != 0) multiGroup = true;
        }
        if (!e->Size) break;
        off += e->Size;
    }
    if (multiGroup) return L"core pinning: skipped (multiple processor groups)";
    off = 0;
    while (off < len) {
        auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) {
            BYTE eff = e->Processor.EfficiencyClass;
            if (eff != top) hybrid = true;
            if (eff == top) {
                KAFFINITY m = e->Processor.GroupMask[0].Mask;
                if (!m) zeroMask = true;
                mask |= m; picked++;
            }
        }
        if (!e->Size) break;
        off += e->Size;
    }
    if (!mask || zeroMask) return L"core pinning: implausible masks, left alone";
    if (!hybrid) return L"core pinning: not a hybrid CPU, left alone";
    if (!SetProcessAffinityMask(GetCurrentProcess(), mask)) return L"core pinning: SetProcessAffinityMask failed";
    wchar_t hx[24]; swprintf_s(hx, L"0x%llX", (unsigned long long)mask);
    return L"core pinning: " + std::to_wstring(picked) + L" P-core logicals (" + hx + L")";
}

// ---------------------------------------------------------------- enumeration

static std::vector<Device> EnumDevices()
{
    std::vector<Device> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            LPWSTR id = nullptr;
            d->GetId(&id);
            std::wstring name = L"(unnamed)";
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    name = pv.pwszVal;
                PropVariantClear(&pv);
                ps->Release();
            }
            out.push_back({ id ? std::wstring(id) : std::wstring(), name });
            if (id) CoTaskMemFree(id);
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

// Minimal manifest read: pull the value of the "name" field out of model.json.
// (Defined below the string helpers; declared here because enumeration needs it.)
static std::string ManifestField(const std::wstring& path, const char* key);
static std::wstring Utf8ToWide(const std::string& s);

static std::vector<Model> EnumModels()
{
    std::vector<Model> out;
    std::wstring pattern = ExeDir() + L"\\models\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring folder = fd.cFileName;
        std::wstring manifest = ExeDir() + L"\\models\\" + folder + L"\\model.json";
        if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::wstring name = Utf8ToWide(ManifestField(manifest, "name"));
        out.push_back({ folder, name.empty() ? folder : name });
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ---------------------------------------------------------------- notification window

struct Notify {
    HWND hwnd = nullptr;
    std::wstring text;
    bool visible = false;
};
static Notify g_notify;
static const wchar_t* kNotifyClass = L"TiroNotifyWindow";

static void HideEyeball();
static void ShowEyeball();

static void HideNotification()
{
    if (!g_notify.hwnd || !g_notify.visible) return;
    KillTimer(g_notify.hwnd, 1);
    ShowWindow(g_notify.hwnd, SW_HIDE);
    g_notify.visible = false;
}

static void PositionNotification()
{
    // Always the primary monitor: the tray lives there, so notifications belong there too.
    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    RECT rc{};
    GetWindowRect(g_notify.hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(g_notify.hwnd, HWND_TOPMOST,
                 mi.rcWork.right - w - 16, mi.rcWork.bottom - h - 16,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

// The Windows app theme (Settings > Personalisation > Colours), not the classic system colours:
// a plain Win32 window is not dark-mode aware, so GetSysColor would paint white on a dark desktop.
static bool SystemUsesDarkTheme()
{
    HKEY k{};
    DWORD v = 1, cb = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&v, &cb);
        RegCloseKey(k);
    }
    return v == 0;
}

static void PaintNotification(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);

    bool dark = SystemUsesDarkTheme();
    HBRUSH bg = CreateSolidBrush(dark ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    HBRUSH frame = CreateSolidBrush(dark ? RGB(86, 86, 86) : GetSysColor(COLOR_WINDOWFRAME));
    FrameRect(dc, &rc, frame);
    DeleteObject(frame);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dark ? RGB(240, 240, 240) : GetSysColor(COLOR_WINDOWTEXT));
    RECT tr = rc;
    InflateRect(&tr, -12, -12);
    DrawTextW(dc, g_notify.text.c_str(), -1, &tr,
              DT_LEFT | DT_WORDBREAK | DT_VCENTER | DT_NOPREFIX);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK NotifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        PaintNotification(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == 1) { HideNotification(); return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void InitNotify()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NotifyProc;
    wc.hInstance = g.hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kNotifyClass;
    RegisterClassExW(&wc);
    g_notify.hwnd = CreateWindowExW(
        // WS_EX_TRANSPARENT: the popup is output-only, so mouse input belongs to
        // whatever is underneath it - it must never swallow a click.
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        kNotifyClass, L"Tiro", WS_POPUP, 0, 0, 360, 80, nullptr, nullptr, g.hInst, nullptr);
}

// Theme-coloured message notification: opaque, no animation, auto-dismiss in 5 s.
static void Notify(const std::wstring& text)
{
    Log(L"INFO", L"notification: " + text);
    HideEyeball();               // a message replaces the status pop-up
    if (!g_notify.hwnd) return;
    g_notify.text = text;

    HDC dc = GetDC(g_notify.hwnd);
    RECT rc{0, 0, 360, 0};
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    ReleaseDC(g_notify.hwnd, dc);

    SetWindowPos(g_notify.hwnd, HWND_TOPMOST, 0, 0, rc.right + 28, rc.bottom + 28,
                 SWP_NOMOVE | SWP_NOACTIVATE);
    PositionNotification();
    InvalidateRect(g_notify.hwnd, nullptr, TRUE);
    ShowWindow(g_notify.hwnd, SW_SHOWNOACTIVATE);
    g_notify.visible = true;
    SetTimer(g_notify.hwnd, 1, 5000, nullptr);
    // No keyboard hook to let Escape dismiss this early: a global WH_KEYBOARD_LL is the
    // #1 behavioural signature of keyloggers, and enterprise EDR reacts to it. The popup
    // auto-dismisses after 5 s instead.
}

// ---------------------------------------------------------------- audio capture

struct Audio {
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    HANDLE evt = nullptr;
    HANDLE thread = nullptr;
    volatile LONG stop = 0;
    double peak = 0.0;
    double rms = 0.0;
    ULONGLONG frames = 0;
};
static Audio g_audio;

static void CloseCapture();
static void SetState(int s);
static void StartEngine();
static void StopEngine();
static bool EngineActive();   // engine thread alive? (g_engine is defined further down)
static void EnginePushSamples(const short* samples, UINT32 count);
static void MarkSessionBoundary();    // dictation-on: boundary without a reset
static void DropPendingAudio();
static HWND PickTarget();
static void QueueInject(const std::wstring& text, bool sep = true);
static void ClearInjectQueue();
static void StartInjector();
static void InstallForegroundHook();
static bool TargetHasFocus();

// ---------------------------------------------------------------- polish (optional post-dictation rewrite)
//
// Off by default and opt-in from the tray (applies on the next start). When it is on, the
// session's raw text is rewritten once by the punctuation model when the user ends dictation,
// and the dictated span is replaced in one atomic paste. Forced stops (focus loss, device
// loss) end dictation with the text exactly as typed. Capture stays closed while it runs.

static std::wstring g_sessionText;              // what Deliver actually handed the target
static std::mutex g_polishMu;
static std::condition_variable g_polishCv;
static bool g_delivering = false;               // a Deliver is inside SendUnicode right now
static std::atomic<bool> g_polishCancel{ false };
static bool g_polishEnabled = false;
static std::atomic<bool> g_sessionPolish{ false };   // this session relies on the punctuation model
static std::atomic<bool> g_probePending{ false };    // capability probe running in the background
static bool g_uttStarted = false;      // anything typed since the session started
                                       // (reset at every TurnOn, not just engine start)
static wchar_t g_lastInjected = 0;     // last char queued for injection (see QueueInject)
static wchar_t g_docTail = 0;          // last char typed, surviving queue clears
static HWND g_prevSessionTarget = nullptr;   // window the last session typed into
static bool TargetSupportsOneShot(HWND top);   // defined with the rewrite paths
struct ModeProbeCtx { HWND target; unsigned long long gen; };
static DWORD WINAPI ModeProbeThread(LPVOID p); // decides it away from the microphone
static std::atomic<unsigned long long> g_sessionGen{ 0 };  // probe results belong to one session

static const wchar_t* OpenErrorMessage(HRESULT hr)
{
    switch (hr) {
    case AUDCLNT_E_DEVICE_IN_USE:
        return L"Could not open the microphone - it may be in use by another application.";
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return L"The Windows audio service is not running.";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        return L"The microphone could not be opened.";
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        return L"The microphone does not support 16 kHz mono audio.";
    default:
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            return L"No microphones available to transcribe.";
        return L"The microphone could not be opened.";
    }
}

static DWORD WINAPI CaptureThread(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        if (InterlockedCompareExchange(&g_audio.stop, 0, 0)) break;
        if (WaitForSingleObject(g_audio.evt, 200) != WAIT_OBJECT_0) continue;

        UINT32 packet = 0;
        if (FAILED(g_audio.cap->GetNextPacketSize(&packet))) {
            PostMessageW(g.hwnd, WM_APP_DEVICE_LOST, 0, 0);
            break;
        }
        while (packet > 0) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            HRESULT hr = g_audio.cap->GetBuffer(&data, &n, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                PostMessageW(g.hwnd, WM_APP_DEVICE_LOST, 0, 0);
                goto done;
            }
            if (FAILED(hr)) break;
            if (data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const short* s = (const short*)data;
                double p = 0.0, sum = 0.0;
                for (UINT32 i = 0; i < n; ++i) {
                    double v = (double)s[i] / 32768.0;
                    sum += v * v;
                    if (v < 0) v = -v;
                    if (v > p) p = v;
                }
                if (p > g_audio.peak) g_audio.peak = p;
                if (n) g_audio.rms = sqrt(sum / n);
                EnginePushSamples(s, n);
            }
            g_audio.frames += n;
            g_audio.cap->ReleaseBuffer(n);
            if (FAILED(g_audio.cap->GetNextPacketSize(&packet))) break;
        }
    }
done:
    CoUninitialize();
    return 0;
}

// 16 kHz mono 16-bit, converted in-box by the audio engine (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM).
static HRESULT OpenCapture(const std::wstring& deviceId)
{
    CloseCapture();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&g_audio.enumr));
    if (FAILED(hr)) return hr;

    hr = deviceId.empty()
       ? g_audio.enumr->GetDefaultAudioEndpoint(eCapture, eConsole, &g_audio.dev)
       : g_audio.enumr->GetDevice(deviceId.c_str(), &g_audio.dev);
    if (FAILED(hr)) return hr;

    hr = g_audio.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               (void**)&g_audio.client);
    if (FAILED(hr)) return hr;

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 16000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    hr = g_audio.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                    0, 0, &fmt, nullptr);
    if (FAILED(hr)) return hr;

    g_audio.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_audio.evt) return E_FAIL;
    hr = g_audio.client->SetEventHandle(g_audio.evt);
    if (FAILED(hr)) return hr;
    hr = g_audio.client->GetService(IID_PPV_ARGS(&g_audio.cap));
    if (FAILED(hr)) return hr;
    hr = g_audio.client->Start();
    if (FAILED(hr)) return hr;

    g_audio.stop = 0;
    g_audio.frames = 0;
    g_audio.peak = 0.0;
    g_audio.thread = CreateThread(nullptr, 0, CaptureThread, nullptr, 0, nullptr);
    return S_OK;
}

static void CloseCapture()
{
    if (g_audio.thread) {
        InterlockedExchange(&g_audio.stop, 1);
        WaitForSingleObject(g_audio.thread, 2000);
        CloseHandle(g_audio.thread);
        g_audio.thread = nullptr;
    }
    if (g_audio.client) g_audio.client->Stop();
    if (g_audio.cap)    { g_audio.cap->Release();    g_audio.cap = nullptr; }
    if (g_audio.client) { g_audio.client->Release(); g_audio.client = nullptr; }
    if (g_audio.dev)    { g_audio.dev->Release();    g_audio.dev = nullptr; }
    if (g_audio.enumr)  { g_audio.enumr->Release();  g_audio.enumr = nullptr; }
    if (g_audio.evt)    { CloseHandle(g_audio.evt);  g_audio.evt = nullptr; }
}

static void TurnOn()
{
    if (EnumDevices().empty()) {
        Log(L"WARN", L"turn on refused: no microphones");
        Notify(L"No microphones available to transcribe.");
        return;
    }
    // The recogniser outlives a toggle, but a model change, a lost capture device or an
    // engine failure stops the engine thread. Without this, turning dictation back on
    // would open the microphone and queue audio that nothing ever decodes.
    if (!EngineActive()) {
        Log(L"INFO", L"engine stopped earlier: restarting it for this session");
        StartEngine();
    }
    HRESULT hr = OpenCapture(g.deviceId);
    if (FAILED(hr) && !g.deviceId.empty() && hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        Log(L"WARN", L"saved microphone not found: falling back to the system default");
        g.deviceId.clear();
        RegSetStr(L"DeviceId", L"");
        hr = OpenCapture(L"");
    }
    if (FAILED(hr)) {
        wchar_t b[128];
        swprintf_s(b, L"capture open failed hr=0x%08X", (unsigned)hr);
        Log(L"ERROR", b);
        SetState(kOff);
        Notify(OpenErrorMessage(hr));
        return;
    }
    MarkSessionBoundary();  // boundary, not a reset: the stream stays warm
    DropPendingAudio();
    ClearInjectQueue();
    { std::lock_guard<std::mutex> lk(g_polishMu); g_sessionText.clear(); }
    g_polishCancel = true;          // if a polish replacement is still typing, stop it now
    g.target = PickTarget();
    {
        wchar_t cls[64]{}; wchar_t title[128]{};
        if (g.target) {
            GetClassNameW(g.target, cls, 64);
            GetWindowTextW(g.target, title, 128);
        }
        Log(L"INFO", std::wstring(L"target window: ") + (g.target ? cls : L"none") + L" \"" +
                     (g.target ? title : L"") + L"\"");
    }
    if (!g.target) {
        CloseCapture();     // never leave the microphone live with nothing to type into
        Notify(L"No window to type into. Click into the document you want to dictate to, then turn dictation on.");
        return;
    }
    if (!TargetHasFocus()) {
        // Turning on from the tray leaves focus on the shell, and the user would then have to
        // click back into their document before anything they say can be typed. We just received
        // that click, so Windows normally grants us the right to hand focus back.
        SetForegroundWindow(g.target);
        Sleep(60);          // the foreground change lands asynchronously; check once it has
        Log(L"INFO", TargetHasFocus() ? L"focus returned to the target window"
                                      : L"focus could not be returned; click into the document to continue");
    }
    g.onAt = GetTickCount64();
    g.targetFocused = TargetHasFocus();
    // One punctuation authority per session (spec 6.7): when the target can take the
    // one-shot rewrite, the punctuation model owns the FINAL form at stop; when it
    // cannot (or the capability cannot be established in time), the speech model's
    // punctuation stands as typed. Live text keeps the model's marks in both cases -
    // nothing is stripped, so a refused rewrite can never leave punctuation lost.
    // The engine lives across sessions, but every session is a new sentence:
    // the session's first word gets its separating space, its capital letter,
    // and the closing period at stop. g_lastInjected deliberately survives the
    // reset - it is how the first word learns whether a space is owed at all.
    // With the decoder retired per session, the model no longer trails a space
    // across the seam, so when resuming in the SAME window the remembered last
    // typed character stands in for it; a different window starts fresh.
    if (g_docTail && g.target == g_prevSessionTarget) g_lastInjected = g_docTail;
    g_prevSessionTarget = g.target;
    g_uttStarted = false;
    // The capability probe must never delay the microphone: accessibility into an
    // Electron window takes up to its 700 ms budget, and every one of those
    // milliseconds is the user's first words unheard (measured live: a session
    // that began speaking instantly lost its first three words to the probe).
    // The probe runs on its own thread now; nothing reads the answer before the
    // stop, and the stop waits for it. Live text is identical in both modes, so
    // the session itself never pauses for this decision.
    g_probePending = true;
    {
        ModeProbeCtx* ctx = new ModeProbeCtx{ g.target, ++g_sessionGen };
        HANDLE probeTh = CreateThread(nullptr, 0, ModeProbeThread, ctx, 0, nullptr);
        if (probeTh) CloseHandle(probeTh);
        else { delete ctx; g_probePending = false; g_sessionPolish = g_polishEnabled && TargetSupportsOneShot(g.target); }
    }
    SetState(kOn);
    Log(L"INFO", L"dictation on: capture open at 16 kHz mono");
    ShowEyeball();
}

// Switching device while listening reopens capture immediately; if the new device will not
// open, the previous one is restored so dictation is never silently lost.
static void ApplyDeviceChange(const std::wstring& newId, const std::wstring& label)
{
    std::wstring prev = g.deviceId;
    bool wasOn = (g.state == kOn);

    g.deviceId = newId;
    RegSetStr(L"DeviceId", g.deviceId);
    Log(L"INFO", L"microphone selected: " + label);

    if (!wasOn) return;

    HRESULT hr = OpenCapture(g.deviceId);
    if (SUCCEEDED(hr)) return;

    Log(L"WARN", L"new device failed to open, restoring previous device");
    g.deviceId = prev;
    RegSetStr(L"DeviceId", g.deviceId);
    if (FAILED(OpenCapture(g.deviceId))) {
        CloseCapture();
        SetState(kOff);
    }
    Notify(OpenErrorMessage(hr));
}


// ---------------------------------------------------------------- the eyeball

struct Eyeball {
    HWND hwnd = nullptr;
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int size = 148;
    bool visible = false;
    double smooth = 0.0;
    double refRms = 0.0;      // ceiling: loudest speech recently seen; decays slowly
    double avgRms = 0.0;      // floor: slow ambience average (hiss tracker)
    ULONGLONG lastVoice = 0;
};
static Eyeball g_eye;
static const wchar_t* kEyeClass = L"TiroEyeballWindow";

static void HideEyeball()
{
    if (!g_eye.hwnd || !g_eye.visible) return;
    KillTimer(g_eye.hwnd, IDT_EYE);
    ShowWindow(g_eye.hwnd, SW_HIDE);
    g_eye.visible = false;
    g_eye.smooth = 0.0;
}

// Draws the eye into the layered DIB and pushes it with per-pixel alpha.
static void RenderEyeball()
{
    if (!g_eye.bits) return;
    int S = g_eye.size;
    auto* px = (unsigned int*)g_eye.bits;
    double cx = (S - 1) / 2.0, cy = (S - 1) / 2.0;
    double R = S * 0.42;
    double lvl = g_eye.smooth;

    for (int y = 0; y < S; ++y) {
        for (int x = 0; x < S; ++x) {
            double dx = x - cx, dy = y - cy;
            double d = sqrt(dx * dx + dy * dy);
            double a = 0.0, r = 0.0, gr = 0.0, b = 0.0;

            // Eyeball geometry, second pass: badge outer edge R; thin metal collar 0.78R..R
            // whose sides fall to near-black and whose top/bottom catch tight bright arcs; a
            // dark inner gap 0.62R..0.78R (the lens sits recessed in its housing); the lens
            // itself holds ~235 red out to t=0.8 then drops to a dark crimson rim, with a
            // tight hot-orange core and a small glass glint near the top edge. No outer halo.
            double Rl = R * 0.52;      // lens
            double Rg = R * 0.80;      // gap outer edge / collar start
            double glow = 0.45 + 0.55 * lvl;
            if (d <= Rl) {
                double t = d / Rl;
                double k = 1.0 - t;
                // The light blooms OUTWARD with level rather than the whole disc brightening
                // evenly: the falloff exponent starts steep (tight deep-red ember at idle) and
                // flattens as the voice rises, so bright orange visibly spreads from the core.
                double p = 3.4 - 2.85 * lvl;
                r  = 35 + 205 * pow(k, p) * (0.72 + 0.28 * glow);
                gr = 10 + 95 * pow(k, p + 1.2) * glow;
                b  = 8 + 12 * pow(k, p + 0.6);
                // Hot pinpoint core: tiny near-white centre, steep falloff.
                double hc = d / (Rl * 0.10);
                if (hc < 1.0) {
                    double s = (1.0 - hc) * (1.0 - hc) * (0.75 + 0.25 * lvl);
                    r  += (255.0 - r)  * s;
                    gr += (210.0 - gr) * s;
                    b  += (140.0 - b)  * s;
                }
                double hc2 = d / (Rl * (0.15 + 0.55 * lvl));  // orange bloom, radius grows with voice
                if (hc2 < 1.0) {
                    double s = (1.0 - hc2) * (1.0 - hc2) * (0.35 + 0.45 * lvl);
                    gr += (175.0 - gr) * s * 0.8;
                }
                // Small glassy specular near the top edge.
                double hdx = x - cx, hdy = y - (cy - Rl * 0.55);
                double hd = sqrt(hdx * hdx + hdy * hdy) / (Rl * 0.16);
                if (hd < 1.0) {
                    double s = (1.0 - hd) * (1.0 - hd) * (0.5 + 0.5 * lvl);
                    r += (255.0 - r) * s; gr += (250.0 - gr) * s; b += (245.0 - b) * s;
                }
                if (r > 255.0) r = 255.0;
                if (gr > 255.0) gr = 255.0;
                if (b > 255.0) b = 255.0;
                a = 255.0;
            } else if (d <= Rg) {
                // dark inner gap: rim crimson bleeding to black housing
                double u = (d - Rl) / (Rg - Rl);
                r = 90.0 * glow * (1.0 - u) * (1.0 - u) + 6.0;
                gr = 4.0; b = 4.0;
                a = 255.0;
            } else if (d <= R) {
                // thin metal collar: near-black sides, tight bright arcs top/bottom
                double tr = (d - Rg) / (R - Rg);
                double ang = fabs(dy) / (d + 1e-9);
                double band = ang * ang; band = band * band * band * band;   // ang^8
                double m = 45 + 120 * band;
                // Convex chrome: bright specular arc on the upper band, soft bounce below.
                double wu = 1.0 - fabs(tr - 0.40) / 0.38;
                if (wu > 0.0 && dy < 0.0) m += 130.0 * wu * (0.25 + 0.75 * band);
                if (wu > 0.0 && dy > 0.0) m += 35.0 * wu * (0.25 + 0.75 * band);
                double w2 = 1.0 - fabs(tr - 0.95) / 0.10;                      // crisp outer rim line
                if (w2 > 0.0) m += 45.0 * w2 * (0.45 + 0.55 * band);
                // Inner shadow seats the pupil - but it must not bite into the bright
                // top/bottom arcs, where a dark band reads as a black smudge on the chrome.
                if (tr < 0.20) {
                    double sh = 1.0 - band;
                    if (sh < 0.0) sh = 0.0;
                    m -= 35.0 * (1.0 - tr / 0.20) * (0.15 + 0.85 * sh);
                }
                if (m < 0.0) m = 0.0;
                r = gr = b = m;
                a = 255.0;
                if (d > R - 1.0) {
                    double aa = (R + 0.5 - d) / 1.5;
                    if (aa < 0.0) aa = 0.0;
                    a = 255.0 * aa;
                }
            }

            if (a <= 0.0) { px[y * S + x] = 0; continue; }
            double af = a / 255.0;
            unsigned int A = (unsigned int)(a + 0.5);
            unsigned int Rr = (unsigned int)(r * af + 0.5);
            unsigned int Gg = (unsigned int)(gr * af + 0.5);
            unsigned int Bb = (unsigned int)(b * af + 0.5);
            px[y * S + x] = (A << 24) | (Rr << 16) | (Gg << 8) | Bb;
        }
    }

    POINT origin{};
    HWND fg = GetForegroundWindow();
    (void)fg;
    POINT zero{0, 0};
    HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    origin.x = mi.rcWork.right - S - 16;
    origin.y = mi.rcWork.bottom - S - 16;

    SIZE size{S, S};
    POINT src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_eye.hwnd, nullptr, &origin, &size, g_eye.memDC, &src, 0, &bf, ULW_ALPHA);
}

static void ShowEyeball()
{
    if (!g_eye.hwnd) return;
    // "Disable Eyeball Animation" in the tray menu: the window is never shown
    // and its render timer never starts, so it costs nothing while disabled.
    if (RegGetDword(L"ShowEye", 1) == 0) return;
    HideNotification();          // a message always wins over the status pop-up
    g_eye.lastVoice = GetTickCount64();
    RenderEyeball();
    ShowWindow(g_eye.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_eye.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_eye.visible = true;
    SetTimer(g_eye.hwnd, IDT_EYE, 33, nullptr);
}

static void EyeballTick()
{
    // The lens travels its FULL range on this mic, whatever the mic's absolute level is.
    // Earlier fixed-gain versions (EyeGain 7, then 16) had to be retuned every time the
    // boom arm moved: a loud mic made the old gain clip, a quiet one (speech RMS ~0.02
    // after the arm moved back) only travelled the bottom tenth. So normalise instead:
    // refRms (loudest recent speech, ceiling tracked in IDT_METER) is full brightness;
    // everything between the hiss floor and the ceiling lands relative inside that range.
    // EyeGain survives as a fine-trim only: 1600 = neutral, 3200 = twice as eager.
    // The ^0.625 curve (x*sqrt(sqrt(x))) then lifts mid-level speech so ordinary
    // conversation visibly sways the lens while shouting still cannot clip it.
    double target = g_audio.rms / (std::max)(g_eye.refRms, 0.012)
                  * RegGetDword(L"EyeGain", 1600) / 1600.0;
    if (target > 1.0) target = 1.0;
    target = target * sqrt(sqrt(target));
    const double quiet = (std::max)(0.004, g_eye.avgRms * 1.4);
    if (g_audio.rms < quiet) target = 0.0;
    const double atk = RegGetDword(L"EyeAttack", 750) / 1000.0;
    const double rel = RegGetDword(L"EyeRelease", 280) / 1000.0;
    // fast attack, slow release
    g_eye.smooth += (target - g_eye.smooth) * (target > g_eye.smooth ? atk : rel);

    if (target > 0.012) g_eye.lastVoice = GetTickCount64();

    RenderEyeball();

    if (GetTickCount64() - g_eye.lastVoice > 5000) HideEyeball();
}

static LRESULT CALLBACK EyeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == IDT_EYE) { EyeballTick(); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void InitEyeball()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EyeProc;             // drives the animation timer; content is pushed
    wc.hInstance = g.hInst;
    wc.lpszClassName = kEyeClass;
    RegisterClassExW(&wc);

    g_eye.hwnd = CreateWindowExW(
        // WS_EX_TRANSPARENT: the eyeball is output-only and covers a corner of the
        // screen, so clicks must fall through to the window under it.
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TRANSPARENT,
        kEyeClass, L"Tiro", WS_POPUP, 0, 0, g_eye.size, g_eye.size,
        nullptr, nullptr, g.hInst, nullptr);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_eye.size;
    bmi.bmiHeader.biHeight = -g_eye.size;     // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    g_eye.memDC = CreateCompatibleDC(screen);
    g_eye.dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_eye.bits, nullptr, 0);
    if (g_eye.dib) SelectObject(g_eye.memDC, g_eye.dib);
    ReleaseDC(nullptr, screen);
}


// ---------------------------------------------------------------- speech engine

extern "C" {
#include "sherpa-onnx-c-api.h"
}

#include <condition_variable>
#include <deque>
#include <mutex>

struct Engine {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<float> q;
    bool stop = false;
    bool active = false;
    HANDLE thread = nullptr;
    const SherpaOnnxOnlineRecognizer* rec = nullptr;
    const SherpaOnnxOnlineStream* stream = nullptr;
    unsigned long long samplesSinceCut = 0;   // for log segments only; the stream is never reset mid-dictation
    size_t cutTyped = 0;                 // g_typed length at the last log split or session start
    // A user-initiated stop drains instead of dropping. Set by StopDictation after
    // the microphone is closed; the inference thread finishes decoding the queue, the injector
    // types it (g_drainActive lets it act while the app is already Off), then the flags clear.
    // drainUntil bounds the drain so a stalled decoder can never type long after the click.
    bool drain = false;
    bool drainPolish = false;
    ULONGLONG drainUntil = 0;
};
static Engine g_engine;
static bool EngineActive() { return g_engine.active; }

// True from the moment a user stop begins until the drained tail has been typed. The injector
// accepts work while this is set even though the state is already Off - that is the whole
// point of the drain - and clears nothing itself; the engine thread owns the lifetime.
static std::atomic<bool> g_drainActive{ false };

// Discards audio captured but not yet decoded, so turning off does not type stale speech.
static void DropPendingAudio()
{
    std::lock_guard<std::mutex> lk(g_engine.mu);
    g_engine.q.clear();
}


// ---------------------------------------------------------------- capture recorder
//
// Diagnostic only, opt-in via HKCU\Software\Tiro\RecordWav (DWORD 1). Writes exactly the 16 kHz
// mono PCM the recogniser receives, so a session can be replayed offline through the same engine
// with different endpoint rules and slice sizes. Nothing is recorded unless the value is set.

static HANDLE g_recFile = INVALID_HANDLE_VALUE;
static DWORD  g_recBytes = 0;
static std::wstring g_recPath;

static void RecorderOpen()
{
    if (g_recFile != INVALID_HANDLE_VALUE) return;
    if (!RegGetDword(L"RecordWav", 0)) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[MAX_PATH];
    swprintf_s(name, L"%s\\capture-%04d%02d%02d-%02d%02d%02d.wav", LocalAppDir().c_str(),
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_recPath = name;
    g_recFile = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_recFile == INVALID_HANDLE_VALUE) {
        Log(L"WARN", L"recorder: cannot create the capture file");
        return;
    }
    g_recBytes = 0;
    unsigned char hdr[44]{};
    memcpy(hdr + 0, "RIFF", 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    *(DWORD*)(hdr + 16) = 16;          // fmt chunk size
    *(WORD*)(hdr + 20) = 1;            // PCM
    *(WORD*)(hdr + 22) = 1;            // mono
    *(DWORD*)(hdr + 24) = 16000;       // sample rate
    *(DWORD*)(hdr + 28) = 32000;       // byte rate
    *(WORD*)(hdr + 32) = 2;            // block align
    *(WORD*)(hdr + 34) = 16;           // bits
    memcpy(hdr + 36, "data", 4);
    DWORD wrote = 0;
    WriteFile(g_recFile, hdr, 44, &wrote, nullptr);
    Log(L"INFO", L"recorder: writing " + g_recPath);
}

static void RecorderWrite(const short* samples, UINT32 count)
{
    if (g_recFile == INVALID_HANDLE_VALUE || !count) return;
    DWORD wrote = 0;
    WriteFile(g_recFile, samples, count * sizeof(short), &wrote, nullptr);
    g_recBytes += wrote;
}

static void RecorderClose()
{
    if (g_recFile == INVALID_HANDLE_VALUE) return;
    DWORD riff = 36 + g_recBytes, data = g_recBytes;
    SetFilePointer(g_recFile, 4, nullptr, FILE_BEGIN);
    DWORD wrote = 0;
    WriteFile(g_recFile, &riff, 4, &wrote, nullptr);
    SetFilePointer(g_recFile, 40, nullptr, FILE_BEGIN);
    WriteFile(g_recFile, &data, 4, &wrote, nullptr);
    CloseHandle(g_recFile);
    g_recFile = INVALID_HANDLE_VALUE;
    Log(L"INFO", L"recorder: closed " + g_recPath + L" (" +
                 std::to_wstring(g_recBytes / 32000) + L" s)");
}

// Called from the capture thread; the inference thread drains the queue.
static void EnginePushSamples(const short* samples, UINT32 count)
{
    if (!g_engine.active || !count) return;
    std::lock_guard<std::mutex> lk(g_engine.mu);
    // Never discard the backlog wholesale: with a decoder slower than real time this queue
    // grows, and clearing it silently deleted up to 20 s of speech the user actually spoke -
    // the largest single source of missing words in a live session, and invisible to every
    // offline replay, which never falls behind. Shed one oldest second at a time and log it,
    // so any loss is bounded and visible instead of silent.
    if (g_engine.q.size() > 16000u * 20u) {
        static long shedSec = 0;
        g_engine.q.erase(g_engine.q.begin(), g_engine.q.begin() + 16000);
        if (++shedSec == 1 || shedSec % 10 == 0)
            Log(L"WARN", L"audio: inference behind, shed " + std::to_wstring(shedSec) +
                L" s of oldest audio in total");
    }
    for (UINT32 i = 0; i < count; ++i) g_engine.q.push_back(samples[i] / 32768.0f);
    g_engine.cv.notify_one();
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), a.data(), n, nullptr, nullptr);
    return a;
}

// Reads a string field out of model.json.
static std::string ManifestField(const std::wstring& path, const char* key)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    char buf[8192]{};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);

    std::string s(buf, got);
    std::string needle = std::string("\"") + key + "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = s.find(':', k);
    if (colon == std::string::npos) return "";
    size_t q1 = s.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return s.substr(q1 + 1, q2 - q1 - 1);
}

// ---------------------------------------------------------------- segment boundary
//
// sherpa-onnx only evaluates its endpoint rule after a decode, and a decode needs a whole
// 560 ms chunk of this model's streaming window. A pause is therefore noticed up to ~0.6 s
// late, and by then the beginning of the user's *next* burst is already inside the stream, so
// OnlineStreamReset throws it away and the user loses their first words. The same reset fired
// on the blanks the model emits inside a phrase lands in the middle of the ongoing burst and
// chops its last words instead. Recorded audio proves both symptoms are this one boundary, and
// that neither the microphone nor the model is dropping words.
//
// So the cut is placed here, where the pause position is known to the sample.
//
// A cut no longer resets the decoder, and this is the reason: recorded audio from a user session
// showed that the first word spoken after a long pause lost its onset, and offline replay with no
// popup at all reproduced it, so neither the microphone nor the eyeball was involved. A freshly
// reset stream emits nothing for roughly its first half-second - it needs several 560 ms chunks
// before it has left-context - and the burst that follows a pause pays for that cold start. The
// stream now runs continuously for the whole dictation, exactly like the reference server's live
// session, and a cut only marks a log line, a space, and the end of a silence measurement. Flush
// pads and refeed existed only to protect against loss *at a reset*; with no reset there is
// nothing to protect, and they are removed rather than defaulted off.
struct Segmenter {
    float silenceRms = 0.01f;   // a frame quieter than this counts as silence
    int   pauseMs    = 2400;    // silence this long ends a log segment
    long  maxSegSamples = 16000 * 20;  // split unbroken speech for the log
    bool  spokeSinceCut = false;  // so a cut fires once, not on every slice of a long silence
    long  silenceRun = 0;        // consecutive silent samples since the last cut

    static Segmenter FromRegistry()
    {
        Segmenter s;
        // Every value below is registry-supplied and therefore arbitrary. Clamp to the
        // range the app can honour, so a mistyped switch degrades instead of
        // overflowing (16000 * MaxUtteranceMs does not fit a long at 4e9) or making
        // every slice a "long" cut (a negative maxSegSamples).
        DWORD pct = RegGetDword(L"SilenceRmsPct", 1);
        if (pct < 1) pct = 1;
        if (pct > 100) pct = 100;
        s.silenceRms = pct / 100.0f;
        s.pauseMs    = (int)RegGetDword(L"PauseMs", 2400);
        if (s.pauseMs < 200) s.pauseMs = 200;
        DWORD maxMs = RegGetDword(L"MaxUtteranceMs", 20000);
        if (maxMs < 1000) maxMs = 1000;
        if (maxMs > 120000) maxMs = 120000;      // ceiling keeps 16000 * maxMs inside a long
        s.maxSegSamples = (long)((long long)16000 * (long long)maxMs);
        return s;
    }

    void Feed(const float* p, size_t n)
    {
        double acc = 0;
        for (size_t i = 0; i < n; ++i) acc += (double)p[i] * p[i];
        float rms = (float)sqrt(acc / (double)(n ? n : 1));
        if (rms >= silenceRms) { silenceRun = 0; spokeSinceCut = true; }
        else                    silenceRun += (long)n;
    }

    // A cut is bookkeeping now, not a state change: `pause` ends a spoken segment, `long` merely
    // splits unbroken speech so the log stays readable. The model's own endpoint rules are no
    // longer consulted - a decoder cut placed mid-phrase used to eat whatever the user said next
    // (the missing "I" in "that I keep my distance"), and with nothing being reset there is no
    // action left for it to take anyway.
    bool Decide(long sinceCutSamples, const wchar_t*& why) const
    {
        if (spokeSinceCut && silenceRun >= (long)pauseMs * 16) { why = L"pause"; return true; }
        if (sinceCutSamples > maxSegSamples)                   { why = L"long";  return true; }
        return false;
    }

    void Restart() { silenceRun = 0; spokeSinceCut = false; }
};

// The stream is no longer reset per clause, so its result is a string that only grows: greedy
// search never revises an emitted token. That makes the delta since the last read exactly the
// text to type, which is how words appear while the user is still speaking instead of after a
// pause - and it means no clause is ever decoded with the decoder's context thrown away.
static std::string g_typed;          // handed to the injector for this utterance
// Turning dictation on must not cold-start the model. The stream is resident, and a reset here
// threw away the left context it had already paid for, so the first half-second of every session
// after the first decoded into nothing. Instead, mark where the running text currently ends:
// whatever the previous session left behind is history, and this session only ever types text
// that appears after the mark.
static void MarkSessionBoundary()
{
    if (!g_engine.rec || !g_engine.stream) { g_engine.cutTyped = 0; return; }
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(g_engine.rec, g_engine.stream);
    if (!r) { g_engine.cutTyped = 0; return; }
    std::string t = r->text ? r->text : "";
    SherpaOnnxDestroyOnlineRecognizerResult(r);
    if (g_typed.size() > t.size()) g_typed = t;   // stream (re)created: g_typed is stale
    g_engine.cutTyped = t.size();
}

// What the decoder hears is what gets typed: no middleware sits between the recogniser and
// the injector. Nemotron punctuates natively, and the optional polish model is the only
// rewriting pass.
static void DeliverStreamDelta()
{
    if (!g_engine.rec || !g_engine.stream) return;
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(g_engine.rec, g_engine.stream);
    std::string cur = (r && r->text) ? r->text : "";
    if (r) SherpaOnnxDestroyOnlineRecognizerResult(r);
    if (cur.size() <= g_typed.size()) {
        if (cur.size() < g_typed.size())
            Log(L"WARN", L"decoder: result shrank mid-utterance; the extra text stays typed");
        return;
    }
    if (cur.compare(0, g_typed.size(), g_typed) != 0) {
        // Cannot untype what is already in the target, so realign and carry on from here.
        Log(L"WARN", L"decoder: result diverged from the typed prefix; skipping the revision");
        g_typed = cur;
        if (g_engine.cutTyped > cur.size()) g_engine.cutTyped = cur.size();  // stream (re)created
        return;
    }
    std::string raw = cur.substr(g_typed.size());
    g_typed = cur;
    if (raw.empty()) return;
    // Raw model output goes straight to the target. Nemotron punctuates natively;
    // the deterministic middleware that once sat here was removed for the first release.
    QueueInject(Utf8ToWide(raw), !g_uttStarted);   // one space between utterances, none inside one
    g_uttStarted = true;
}

static bool CreateRecognizer()
{
    Log(L"INFO", PinToPcores());
    if (g_engine.rec) return true;      // resident across toggles

    std::wstring folder = g.modelFolder;
    if (folder.empty()) {
        auto models = EnumModels();
        if (models.empty()) return false;
        folder = models[0].folder;
    }
    std::wstring dir = ExeDir() + L"\\models\\" + folder;
    std::wstring manifest = dir + L"\\model.json";

    std::string enc = ManifestField(manifest, "encoder");
    std::string dec = ManifestField(manifest, "decoder");
    std::string joi = ManifestField(manifest, "joiner");
    std::string tok = ManifestField(manifest, "tokens");
    // The manifest is data, not code: accept only a plain file name in the folder the
    // manifest came from. No ..\, no drive, no separators - a model folder is not a
    // route to the rest of the disk.
    auto plainName = [](const std::string& f) {
        return !f.empty() && f.find("..") == std::string::npos &&
               f.find('\\') == std::string::npos && f.find('/') == std::string::npos &&
               f.find(':') == std::string::npos;
    };
    if (enc.empty() || dec.empty() || joi.empty() || tok.empty() ||
        !plainName(enc) || !plainName(dec) || !plainName(joi) || !plainName(tok)) {
        Log(L"ERROR", L"model manifest is missing or names files outside its model folder");
        return false;
    }

    std::string encPath = WideToUtf8(dir + L"\\" + Utf8ToWide(enc));
    std::string decPath = WideToUtf8(dir + L"\\" + Utf8ToWide(dec));
    std::string joiPath = WideToUtf8(dir + L"\\" + Utf8ToWide(joi));
    std::string tokPath = WideToUtf8(dir + L"\\" + Utf8ToWide(tok));

    SherpaOnnxOnlineRecognizerConfig cfg{};
    cfg.feat_config.sample_rate = 16000;
    cfg.feat_config.feature_dim = 80;
    cfg.model_config.transducer.encoder = encPath.c_str();
    cfg.model_config.transducer.decoder = decPath.c_str();
    cfg.model_config.transducer.joiner = joiPath.c_str();
    cfg.model_config.tokens = tokPath.c_str();
    // sherpa-onnx applies num_threads to each of the encoder, decoder and joiner sessions, so
    // the process ends up with 3x this many inference threads. Default to half the P-cores and
    // let the user override; a machine that saturates here freezes the desktop.
    {
        // One pinned P-core thread decodes this model in ~0.4 s per second of audio: 2.5x
        // faster than speech arrives, which is enough margin for live dictation. More threads
        // are faster (4 reach ~0.15 s/s) but spin-wait and burn CPU for no user-visible gain.
        // Set the Threads DWORD only if typing ever trails the voice. (The freeze that once
        // came with high thread counts was ORT threads spinning on E-cores; PinToPcores()
        // above removes that failure mode, not the thread count itself.)
        // Clamped: the value comes from the registry, so it can be any DWORD - a mistyped
        // switch must degrade, not hand a negative thread count to the engine.
        DWORD t = RegGetDword(L"Threads", 0);
        if (t > 64) t = 64;
        cfg.model_config.num_threads = t ? (int)t : 1;
    }
    cfg.model_config.provider = "cpu";
    cfg.model_config.debug = 0;
    cfg.model_config.model_type = "transducer";
    cfg.decoding_method = "greedy_search";
    cfg.max_active_paths = 4;
    cfg.enable_endpoint = 1;
    // Overridable so a recorded session can be swept offline without rebuilding.
    cfg.rule1_min_trailing_silence = RegGetDword(L"Rule1Ms", 2500) / 1000.0f;
    cfg.rule2_min_trailing_silence = RegGetDword(L"Rule2Ms", 2500) / 1000.0f;
    cfg.rule3_min_utterance_length = RegGetDword(L"Rule3Ms", 15000) / 1000.0f;

    ULONGLONG t0 = GetTickCount64();
    g_engine.rec = SherpaOnnxCreateOnlineRecognizer(&cfg);
    if (!g_engine.rec) {
        Log(L"ERROR", L"recogniser creation failed");
        return false;
    }
    g_engine.stream = SherpaOnnxCreateOnlineStream(g_engine.rec);
    g_typed.clear();
    g_uttStarted = false;
    // (raw output - no middleware state to reset here)
    if (!g_engine.stream) {
        Log(L"ERROR", L"stream creation failed");
        return false;
    }
    wchar_t b[128];
    swprintf_s(b, L"engine ready in %llu ms (threads=%d)",
               (unsigned long long)(GetTickCount64() - t0), cfg.model_config.num_threads);
    Log(L"INFO", b);
    return true;
}

static void DestroyRecognizer()
{
    if (g_engine.stream) { SherpaOnnxDestroyOnlineStream(g_engine.stream); g_engine.stream = nullptr; }
    if (g_engine.rec)    { SherpaOnnxDestroyOnlineRecognizer(g_engine.rec); g_engine.rec = nullptr; }
}

static DWORD WINAPI EngineThread(LPVOID);
// Declared before EngineThread: the drain-completion wait inspects the inject queue.
static std::deque<std::wstring> g_injectQ;
static std::mutex g_injectMu;

static DWORD WINAPI EngineThread(LPVOID)
{
    if (!CreateRecognizer()) {
        PostMessageW(g.hwnd, WM_APP_ENGINE_FAIL, 0, 0);
        return 0;
    }
    if (!g_engine.stream) {
        g_engine.stream = SherpaOnnxCreateOnlineStream(g_engine.rec);
    g_typed.clear();
    g_uttStarted = false;
    // (raw output - no middleware state to reset here)
        if (!g_engine.stream) {
            PostMessageW(g.hwnd, WM_APP_ENGINE_FAIL, 0, 0);
            return 0;
        }
    }
    PostMessageW(g.hwnd, WM_APP_ENGINE_READY, 0, 0);

    std::vector<float> buf;
    Segmenter seg = Segmenter::FromRegistry();
    int cutByBoundary = 0, cutByLong = 0;
    size_t& cutTyped = g_engine.cutTyped;  // persists across on/off toggles; stream is continuous
    for (;;) {
        bool drainDone = false, drainPolish = false;
        {
            std::unique_lock<std::mutex> lk(g_engine.mu);
            g_engine.cv.wait_for(lk, std::chrono::milliseconds(50),
                                 [] { return g_engine.stop || !g_engine.q.empty(); });
            if (g_engine.stop) break;
            if (g_engine.drain && g_engine.q.empty()) {
                // User stopped; everything captured so far is now decoded. Release the
                // middleware after the drained words so the punctuation lands in order.
                // And wrap up. A finished-flag alone decodes nothing - the transducer
                // is never "ready" once samples run dry - so hand it half a second of silence:
                // the endpointer fires, the lookahead completes, and the last words come out of
                // the normal path. Then declare the input finished for good, flush once more,
                // and deliver everything through the still-open drain. The stream is then
                // retired: each session starts on a clean decoder, so a previous session's
                // trailing marks or a long day's drift can never leak into the next sentence.
                {
                    std::vector<float> sil(8000, 0.0f);   // 500 ms at 16 kHz
                    SherpaOnnxOnlineStreamAcceptWaveform(g_engine.stream, 16000, sil.data(), (int)sil.size());
                    while (SherpaOnnxIsOnlineStreamReady(g_engine.rec, g_engine.stream))
                        SherpaOnnxDecodeOnlineStream(g_engine.rec, g_engine.stream);
                    DeliverStreamDelta();
                }
                SherpaOnnxOnlineStreamInputFinished(g_engine.stream);
                while (SherpaOnnxIsOnlineStreamReady(g_engine.rec, g_engine.stream))
                    SherpaOnnxDecodeOnlineStream(g_engine.rec, g_engine.stream);
                DeliverStreamDelta();
                SherpaOnnxDestroyOnlineStream(g_engine.stream);
                g_engine.stream = SherpaOnnxCreateOnlineStream(g_engine.rec);
                g_typed.clear();
                g_engine.cutTyped = 0;
                g_engine.drain = false;
                drainDone = true;
                drainPolish = g_engine.drainPolish;
                g_engine.drainPolish = false;
            } else if (g_engine.drain && GetTickCount64() > g_engine.drainUntil) {
                g_engine.drain = false;
                drainPolish = g_engine.drainPolish;
                g_engine.drainPolish = false;
                lk.unlock();
                Log(L"WARN", L"drain: deadline passed, the rest of the backlog is dropped");
                DropPendingAudio();
                lk.lock();
                // The decoder consumed some of a backlog that never finished arriving;
                // it is not a safe foundation for the next sentence either. Clean slate.
                if (g_engine.stream) {
                    SherpaOnnxDestroyOnlineStream(g_engine.stream);
                    g_engine.stream = SherpaOnnxCreateOnlineStream(g_engine.rec);
                    g_typed.clear();
                    g_engine.cutTyped = 0;
                }
                drainDone = true;
            }
            buf.assign(g_engine.q.begin(), g_engine.q.end());
            g_engine.q.clear();
        }
        // Buffer the whole queue, decode it in slices below.

        // Feed in small slices and test the endpoint after each one. Accepting the whole batch
        // and then resetting discards every sample past the endpoint point — and those samples
        // are the beginning of the next burst, so the user loses their first words. The loss is
        // bounded by the slice size instead: 1600 samples is 100 ms.
        size_t slice = (size_t)RegGetDword(L"SliceMs", 100) * 16;
        if (slice < 160) slice = 160;
        if (slice > 16000) slice = 16000;      // registry value is arbitrary: cap at 1 s
        size_t pos = 0;
        while (pos < buf.size()) {
            size_t n = (std::min)(slice, buf.size() - pos);
            size_t at = pos;
            // Recorded at the point of acceptance, so the file is exactly what the recogniser
            // received no matter which path produced it.
            {
                std::vector<short> pcm(n);
                for (size_t i = 0; i < n; ++i) {
                    float v = buf[pos + i];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    pcm[i] = (short)(v * 32767.0f);
                }
                RecorderWrite(pcm.data(), (UINT32)n);
            }
            SherpaOnnxOnlineStreamAcceptWaveform(g_engine.stream, 16000, buf.data() + pos, (int)n);
            pos += n;
            g_engine.samplesSinceCut += n;
            seg.Feed(buf.data() + at, n);

            while (SherpaOnnxIsOnlineStreamReady(g_engine.rec, g_engine.stream))
                SherpaOnnxDecodeOnlineStream(g_engine.rec, g_engine.stream);

            DeliverStreamDelta();

            // The energy boundary closes a log segment. Nothing is reset, so nothing is dropped
            // and the next burst starts warm. Deliver first: the words just spoken must be typed
            // before the log line claims them.
            const wchar_t* why = L"";
            bool cut = seg.Decide((long)g_engine.samplesSinceCut, why);
            if (cut) { if (wcscmp(why, L"pause") == 0) cutByBoundary++; else cutByLong++; }

            if (cut) {
                DeliverStreamDelta();
                if (g_typed.size() > cutTyped) {
                    std::string segText = g_typed.substr(cutTyped);
                    double secs = (double)g_engine.samplesSinceCut / 16000.0;
                    wchar_t b[48];
                    swprintf_s(b, L" (%.2fs, %s)", secs, why);
                    // Text only when the diagnostic switch is on; app.log never carries speech
                    // by default.
                    std::wstring head = L"utterance" + std::wstring(b) + L": ";
                    Log(L"INFO", LogTextEnabled()
                        ? head + Utf8ToWide(segText)
                        : head + std::to_wstring(segText.size()) + L" chars");
                    cutTyped = g_typed.size();
                }
                g_engine.samplesSinceCut = 0;
                seg.Restart();
            }
        }

        if (drainDone) {
            // The drain is only over once the words are actually IN the document.
            // Wait (bounded) for the injector to finish - queue empty and nothing mid-SendInput -
            // then stop accepting drain-time injection and arm the polish pass, which rewrites
            // exactly what was delivered and so must see the tail too.
            ULONGLONG dl = GetTickCount64() + 3000;
            for (;;) {
                bool idle = false;
                { std::lock_guard<std::mutex> il(g_injectMu); idle = g_injectQ.empty(); }
                {
                    std::lock_guard<std::mutex> pl(g_polishMu);
                    if (idle && !g_delivering) break;
                }
                if (GetTickCount64() > dl) {
                    Log(L"WARN", L"drain: injector still busy, the tail may arrive late");
                    break;
                }
                Sleep(20);
            }
            // Speech-mode contract: stopping dictation means a sentence ended
            // somewhere the user heard one. When the last character typed was a
            // word character, the sentence is owed its closing period. This goes
            // through the injector while the drain is still active (after it
            // releases, the injector refuses all text by design), and the drain
            // stays open until the period is actually in the document.
            // The mode was decided on the microphone's behalf, possibly still in
            // flight. Wait for the verdict before choosing which writer closes
            // the sentence - it cannot outlast the probe's own 700 ms budget.
            for (ULONGLONG pw = GetTickCount64() + 1500; g_probePending && GetTickCount64() < pw; )
                Sleep(20);
            if (g_probePending) Log(L"WARN", L"polish: capability probe still running at stop; assuming speech mode");
            // A stop that beat the probe armed the drain with the provisional
            // answer; re-read the verdict now that the wait is over, so a
            // supported target never loses its rewrite to a stopwatch.
            if (g_polishEnabled && g_sessionPolish) drainPolish = true;
            if (!g_sessionPolish && g_uttStarted) {
                wchar_t last = 0;
                { std::lock_guard<std::mutex> il(g_injectMu); last = g_lastInjected; }
                if (iswalnum(last)) {
                    QueueInject(L".", false);
                    Log(L"INFO", L"session end: closing period added");
                    ULONGLONG pl = GetTickCount64() + 800;
                    for (;;) {
                        bool idle = false;
                        { std::lock_guard<std::mutex> il(g_injectMu); idle = g_injectQ.empty(); }
                        {
                            std::lock_guard<std::mutex> pl2(g_polishMu);
                            if (idle && !g_delivering) break;
                        }
                        if (GetTickCount64() > pl) {
                            Log(L"WARN", L"session end: closing period still queued at drain close");
                            break;
                        }
                        Sleep(20);
                    }
                }
            }
            g_drainActive = false;
            Log(L"INFO", L"drain: tail released after user stop");
            if (drainPolish) PostMessageW(g.hwnd, WM_APP_POLISH, 0, 0);
        }
    }

    Log(L"INFO", L"log segments: " + std::to_wstring(cutByBoundary) +
                 L" by silence, " + std::to_wstring(cutByLong) + L" by length; stream resets: 0");

    if (g_engine.stream) {
        SherpaOnnxDestroyOnlineStream(g_engine.stream);
        g_engine.stream = nullptr;
    }
    return 0;
}

static void UnloadEngine()
{
    StopEngine();
    if (g_engine.rec) {
        SherpaOnnxDestroyOnlineRecognizer(g_engine.rec);
        g_engine.rec = nullptr;
        Log(L"INFO", L"engine unloaded");
    }
}

static void StartEngine()
{
    {
        std::lock_guard<std::mutex> lk(g_engine.mu);
        g_engine.stop = false;
        g_engine.drain = false;       // a drain from the last user-stop must not leak into this session
        g_engine.drainPolish = false;
        g_engine.q.clear();
    }
    g_drainActive = false;            // a new session never types the previous tail
    if (g_engine.active) return;
    g_engine.active = true;
    RecorderOpen();
    g_engine.thread = CreateThread(nullptr, 0, EngineThread, nullptr, 0, nullptr);
    Log(L"INFO", L"engine thread started");
}

static void StopEngine()
{
    if (!g_engine.active) return;
    {
        std::lock_guard<std::mutex> lk(g_engine.mu);
        g_engine.stop = true;
    }
    g_drainActive = false;
    g_engine.cv.notify_all();
    if (g_engine.thread) {
        WaitForSingleObject(g_engine.thread, 15000);
        CloseHandle(g_engine.thread);
        g_engine.thread = nullptr;
    }
    g_engine.active = false;
    RecorderClose();
    Log(L"INFO", L"engine stopped");
}

// ---------------------------------------------------------------- self test

static bool ReadWav16kMono(const std::wstring& path, std::vector<float>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    std::vector<char> data(size);
    DWORD got = 0;
    ReadFile(h, data.data(), size, &got, nullptr);
    CloseHandle(h);
    if (got < 44) return false;

    auto tag = [&](size_t p, const char* s) { return memcmp(data.data() + p, s, 4) == 0; };
    if (!tag(0, "RIFF") || !tag(8, "WAVE")) return false;

    size_t p = 12;
    int channels = 1, rate = 16000, bits = 16;
    size_t dataOff = 0, dataLen = 0;
    while (p + 8 <= got) {
        DWORD len = *(DWORD*)(data.data() + p + 4);
        if (tag(p, "fmt ")) {
            // channels/rate/bits sit at +10/+12/+22 inside the chunk: refuse a header
            // that is shorter than the fields being read from it.
            if (len < 16 || p + 8 + 16 > (size_t)got) return false;
            channels = *(short*)(data.data() + p + 10);
            rate     = *(int*)(data.data() + p + 12);
            bits     = *(short*)(data.data() + p + 22);
        } else if (tag(p, "data")) {
            dataOff = p + 8;
            // The declared length is a claim about the file, not a fact: a truncated
            // download or a crafted file can promise gigabytes. Trust only what was
            // actually read, so the sample loop can never walk off the buffer.
            size_t avail = (size_t)got - dataOff;
            dataLen = ((size_t)len < avail) ? (size_t)len : avail;
        }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!dataOff || channels != 1 || rate != 16000 || bits != 16) return false;

    const short* s = (const short*)(data.data() + dataOff);
    size_t n = dataLen / 2;
    if (!n) return false;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = s[i] / 32768.0f;
    return true;
}

// Feeds a WAV through the same recogniser and endpointing used live, and logs the transcript.
static void RunSelfTest(const std::wstring& wav)
{
    std::vector<float> samples;
    if (!ReadWav16kMono(wav, samples)) {
        Log(L"ERROR", L"selftest: expected 16 kHz mono 16-bit WAV");
        return;
    }
    if (!CreateRecognizer()) {
        Log(L"ERROR", L"selftest: recogniser failed");
        return;
    }
    Log(L"INFO", L"selftest: " + std::to_wstring(samples.size() / 16000) + L" s of audio");

    // Mirror of the live engine loop: same slice size, same boundary detector, same delivery,
    // so replaying a recorded session here proves exactly what the injector would have received.
    Segmenter seg = Segmenter::FromRegistry();
    long fedSince = 0;
    size_t cutTyped = 0;
    size_t pos = 0;
    while (pos < samples.size()) {
        size_t chunk = (size_t)RegGetDword(L"ChunkMs", 100) * 16;
        if (chunk < 160) chunk = 160;
        if (chunk > 16000) chunk = 16000;      // registry value is arbitrary: cap at 1 s
        size_t n = (std::min)(chunk, samples.size() - pos);
        size_t at = pos;
        SherpaOnnxOnlineStreamAcceptWaveform(g_engine.stream, 16000, samples.data() + pos, (int)n);
        pos += n;
        seg.Feed(samples.data() + at, n);
        fedSince += (long)n;
        while (SherpaOnnxIsOnlineStreamReady(g_engine.rec, g_engine.stream))
            SherpaOnnxDecodeOnlineStream(g_engine.rec, g_engine.stream);

        const wchar_t* why = L"";
        bool cut = seg.Decide(fedSince, why);
        if (cut) {
            const SherpaOnnxOnlineRecognizerResult* r =
                SherpaOnnxGetOnlineStreamResult(g_engine.rec, g_engine.stream);
            if (r) {
                std::string t = r->text ? r->text : "";
                SherpaOnnxDestroyOnlineRecognizerResult(r);
                if (t.size() > cutTyped) {          // continuous stream: log the new part only
                    Log(L"INFO", L"selftest segment [" + std::wstring(why) + L"]: " +
                        Utf8ToWide(t.substr(cutTyped)));
                    cutTyped = t.size();
                }
            }
            seg.Restart();
            fedSince = 0;
        }
    }
    SherpaOnnxOnlineStreamInputFinished(g_engine.stream);
    while (SherpaOnnxIsOnlineStreamReady(g_engine.rec, g_engine.stream))
        SherpaOnnxDecodeOnlineStream(g_engine.rec, g_engine.stream);
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(g_engine.rec, g_engine.stream);
    if (r) {
        std::string tailText = r->text ? r->text : "";
        SherpaOnnxDestroyOnlineRecognizerResult(r);
        if (!tailText.empty()) Log(L"INFO", L"selftest final: " + Utf8ToWide(tailText));
    }
    DestroyRecognizer();
    Log(L"INFO", L"selftest: done");
}


// ---------------------------------------------------------------- live-path self test

static std::vector<float> g_feeder;

static DWORD WINAPI FeederThread(LPVOID)
{
    size_t pos = 0;
    while (pos < g_feeder.size()) {
        size_t n = (std::min)((size_t)3200, g_feeder.size() - pos);
        {
            std::lock_guard<std::mutex> lk(g_engine.mu);
            for (size_t i = 0; i < n; ++i) g_engine.q.push_back(g_feeder[pos + i]);
            g_engine.cv.notify_one();
        }
        pos += n;
        Sleep(200);            // 200 ms of audio per 200 ms: real-time pacing
    }
    Sleep(4000);               // let endpointing finalise and inject
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    return 0;
}


// ---------------------------------------------------------------- injector
//
// Injection never runs on the UI thread: SendInput blocks when the target's input queue
// stalls (a window storm, a hung app), and a blocked UI thread freezes the tray, the meter
// and any installed low-level keyboard hook — which stalls input system-wide. A dedicated
// thread absorbs that, and the queue is bounded so a stalled target cannot grow memory.

// g_injectQ / g_injectMu are declared above, before EngineThread.
static std::condition_variable g_injectCv;
static bool g_injectStop = false;
static HANDLE g_injectThread = nullptr;


static bool ProcessImageIs(HWND h, const wchar_t* name)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != 0;
    CloseHandle(p);
    if (!ok) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return !_wcsicmp(base, name);
}

// The desktop itself. It is a real place to click, so it ends dictation, but it is never a target.
static bool IsDesktopWindow(HWND h)
{
    if (!h) return false;
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

// The taskbar, its flyouts and Tiro's own windows. Focus moving to one of these is not the user
// leaving their document — it is how they reach Tiro.
//
// Matching class names alone is not enough: the Windows 11 overflow flyout and task view use
// classes that change between builds, and a miss here pins the flyout as the dictation target
// and stops dictation the moment the user clicks back into their document. So anything owned by
// explorer.exe counts, except real folder windows, which are legitimate targets.
static bool IsTraySurface(HWND h)
{
    if (!h) return true;
    if (h == g.hwnd || h == g_eye.hwnd || h == g_notify.hwnd) return true;
    if (IsDesktopWindow(h)) return false;        // clicking the desktop IS leaving the document
    wchar_t cls[64]{};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") ||
        !wcscmp(cls, L"NotifyIconOverflowWindow") || !wcscmp(cls, L"Windows.UI.Core.CoreWindow") ||
        !wcscmp(cls, L"XamlExplorerHostIslandWindow") || !wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland"))
        return true;
    if (!wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"ExploreWClass"))
        return false;                            // a File Explorer folder window is a real target
    return ProcessImageIs(h, L"explorer.exe");   // task view, search, flyouts, shell chrome
}

static bool IsShellWindow(HWND h)
{
    return IsTraySurface(h) || IsDesktopWindow(h);
}

// Runs four times a second from the message loop. It remembers the last real application window
// (so turning dictation on from the tray pins the application, not the shell) and notices when
// focus leaves the pinned target. Polling rather than SetWinEventHook: an out-of-context hook
// installed from a tray-only process is not reliably delivered, and a stale lastApp silently
// pins the wrong window.
static std::wstring FgProcessName(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return L"?";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"pid " + std::to_wstring(pid);
    wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH;
    std::wstring r = QueryFullProcessImageNameW(p, 0, path, &n) ? wcsrchr(path, L'\\') + 1 : L"?";
    CloseHandle(p);
    return r;
}

static void WatchForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    if (!IsShellWindow(fg)) g.lastApp = fg;          // the desktop is never a dictation target

    if (g.state != kOn || !g.target) return;

    if (fg == g.target || IsChild(g.target, fg)) {
        g.targetFocused = true;
        return;
    }

    // Reaching for the tray is how the user turns dictation off, so it counts as leaving the
    // document — but only once the document has actually had focus, and never in the instant
    // after turning on, while the tray flyout is still closing.
    if (IsTraySurface(fg)) {
        if (!g.targetFocused) return;
        if (GetTickCount64() - g.onAt < 800) return;
        // Deliberately no softer path here, including for the shell's own surfaces. A version of
        // this paused typing (without buffering) while a tray surface held the foreground and
        // stopped only after SoftLossMs; it was verified working, and it was rejected on UX
        // grounds: a user mid-flow cannot tell that a few words were quietly skipped, so pausing
        // loses text invisibly. One rule with no exceptions - focus moves, dictation stops, the
        // notification says so - is teachable. This does mean Explorer's tray-overflow notice
        // (~2 s after an app's first tray icon in a session) stops the first dictation after a
        // reboot; accepted, because the notification explains it.
    }

    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    Log(L"INFO", std::wstring(L"focus left the target window (now: ") + cls +
                 L", " + FgProcessName(fg) + L")");
    PostMessageW(g.hwnd, WM_APP_FOCUS_LOST, 0, 0);
}

static void InstallForegroundHook()
{
    WatchForeground();                                // seed lastApp
    SetTimer(g.hwnd, IDT_WATCH, 250, nullptr);
}

static HWND PickTarget()
{
    HWND fg = GetForegroundWindow();
    if (!IsShellWindow(fg)) return fg;      // toggled from the app itself
    return g.lastApp;                       // toggled from the tray: use the last real window
}

static void SendUnicode(const std::wstring& text, int gapMs = 0)
{
    for (wchar_t ch : text) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        if (gapMs) Sleep((DWORD)gapMs);
    }
}

static bool TargetHasFocus()
{
    if (!g.target || !IsWindow(g.target)) return false;
    HWND fg = GetForegroundWindow();
    return fg == g.target || IsChild(g.target, fg);
}

// Text is typed only into the pinned window, and only while it still has focus. There is no
// holding area: if the target is not focused the segment is discarded, because dictation has
// already stopped and nothing should be typed into a window the user has moved away from.
static void Deliver(const std::wstring& text)
{
    if (!g.target || !IsWindow(g.target)) {
        Log(L"WARN", L"inject: target window is gone, discarding");
        return;
    }
    if (!TargetHasFocus()) {
        Log(L"WARN", L"inject: target lost focus, discarding segment");
        return;
    }
    { std::unique_lock<std::mutex> lk(g_polishMu); g_delivering = true; }
    SendUnicode(text);
    {
        std::unique_lock<std::mutex> lk(g_polishMu);
        g_sessionText += text;              // the replacement pass deletes exactly this back
        g_delivering = false;
    }
    g_polishCv.notify_all();
}

// The stop hotkey races the last keystrokes: while Ctrl/Alt/Shift/Win are logically
// held, the OS reinterprets injected characters as shortcut attempts - Word eats them
// silently, AltGr-flavoured layouts translate them into other characters. Any text
// typed near a hotkey press must therefore wait for the modifiers to come up. When
// nothing is held (almost always) this returns on the first check.
static void WaitForModifierRelease()
{
    static const int mods[] = { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN };
    ULONGLONG t0 = GetTickCount64();
    bool held = false;
    for (;;) {
        held = false;
        for (int m : mods)
            if (GetAsyncKeyState(m) & 0x8000) { held = true; break; }
        if (!held || GetTickCount64() - t0 > 1500) break;
        Sleep(10);
    }
    if (held) Log(L"WARN", L"inject: modifier still held after 1.5s, typing anyway");
}

static DWORD WINAPI InjectorThread(LPVOID)
{
    for (;;) {
        std::wstring text;
        {
            std::unique_lock<std::mutex> lk(g_injectMu);
            g_injectCv.wait(lk, [] { return g_injectStop || !g_injectQ.empty(); });
            if (g_injectStop) return 0;
            text = g_injectQ.front();
            g_injectQ.pop_front();
        }
        const bool draining = g_drainActive && g.state != kOn;
        if (g.state != kOn && !draining) continue;   // never type into a window after dictation stopped
        if (!g.target || !IsWindow(g.target)) {
            Log(L"WARN", L"inject: target window is gone, discarding");
            continue;
        }
        // Keystrokes go to whatever is front at the instant of typing, so the target must be
        // front. While draining, a tray stop has already moved focus to the shell: take focus
        // back for the tail - the same allowed path TurnOn uses. If focus went to a real other
        // window, the user meant to leave it, so the tail is dropped rather than typed there.
        HWND ifg = GetForegroundWindow();
        if (ifg != g.target && !IsChild(g.target, ifg)) {
            if (!draining || !IsShellWindow(ifg)) {
                Log(L"WARN", L"injection skipped: target window is not foreground");
                continue;
            }
            SetForegroundWindow(g.target);
            Sleep(60);
            ifg = GetForegroundWindow();
            if (ifg != g.target && !IsChild(g.target, ifg)) {
                Log(L"WARN", L"inject: could not return focus for the tail, discarding");
                continue;
            }
            Log(L"INFO", L"inject: focus returned to the target for the drained tail");
        }
        WaitForModifierRelease();   // the tail must land as text, never as shortcuts
        ULONGLONG t0 = GetTickCount64();
        Deliver(text);
        ULONGLONG dt = GetTickCount64() - t0;
        if (dt > 1000) Log(L"WARN", L"inject: " + std::to_wstring(dt) + L" ms to deliver a segment");
    }
}

static void StartInjector()
{
    if (g_injectThread) return;
    g_injectStop = false;
    g_injectThread = CreateThread(nullptr, 0, InjectorThread, nullptr, 0, nullptr);
}

// One space between segments is a delimiter, not formatting: nothing is added
// inside a segment, nothing at the start of a session, and nothing if a piece
// already begins or the last one ended with whitespace. The speech model's own
// punctuation flows through untouched in every mode - see QueueInject.

static void QueueInject(const std::wstring& text, bool sep)
{
    if (text.empty()) return;
    std::wstring out = text;
    if (!g_uttStarted) {
        // The decoder stream runs on across sessions; the '.' that finally closes
        // the previous utterance frequently arrives as this session's first token.
        // Tiro already put that sentence's period in at stop, so a leading dot is
        // a duplicate of the mark sitting in the document - drop it, keep one
        // space so the seam stays readable. A new sentence never legitimately
        // starts with '.', so this cannot eat anything the user meant.
        if (g_docTail == L'.' || g_docTail == L'!' || g_docTail == L'?') {
            std::wstring kept;
            size_t i = 0;
            for (; i < out.size(); ++i) {
                if (out[i] != L'.' && !iswspace(out[i])) break;
                if (iswspace(out[i]) && kept.empty()) kept.push_back(L' ');
            }
            kept += out.substr(i);
            if (kept.empty()) return;   // the chunk was only the previous session's mark
            out.swap(kept);
        }
        // Speech-mode contract, one sentence per session: because Tiro closes
        // every session with a period (see the drain end), it also opens one.
        // Capitalize the first letter the session types, whatever Nemotron sent;
        // when the target takes the rewrite path, the polished form simply
        // repeats what is already there.
        for (wchar_t& ch : out)
            if (iswalpha(ch)) { ch = (wchar_t)towupper(ch); break; }
    }
    {
        std::lock_guard<std::mutex> lk(g_injectMu);
        bool needSpace = sep && g_lastInjected != 0 && !iswspace(g_lastInjected) && !iswspace(text[0]);
        if (needSpace) out.insert(out.begin(), L' ');
        g_lastInjected = out.back();
        g_docTail = out.back();
        if (g_injectQ.size() >= 16) {
            g_injectQ.pop_front();
            Log(L"WARN", L"inject: queue full, dropped the oldest segment");
        }
        g_injectQ.push_back(out);
        // What the app decided to type. The words themselves are logged only when the
        // LogText diagnostic switch is on; by default only the length appears, so app.log
        // never contains speech.
        Log(L"INFO", LogTextEnabled()
            ? std::wstring(L"inject: ") + out
            : L"inject: " + std::to_wstring(out.size()) + L" chars");
    }
    g_injectCv.notify_one();
}

static void ClearInjectQueue()
{
    std::lock_guard<std::mutex> lk(g_injectMu);
    g_injectQ.clear();
    g_lastInjected = 0;
}

// ---------------------------------------------------------------- tray

static void UpdateTray()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g.icon[g.state];
    const wchar_t* tip = (g.state == kOn) ? L"Tiro - listening"
                       : (g.state == kOff) ? L"Tiro - off"
                                           : L"Tiro - starting";
    wcscpy_s(nid.szTip, tip);

    if (g.trayAdded) {
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
            Log(L"WARN", L"Shell_NotifyIcon(NIM_MODIFY) failed");
    } else if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g.trayAdded = true;
    } else {
        Log(L"ERROR", L"Shell_NotifyIcon(NIM_ADD) failed - tray icon not created");
    }
}

static void SetState(int s)
{
    g.state = s;
    // The status pop-up belongs to the On state only. Hiding it here means no exit path can
    // forget: dialogs, device loss, focus loss and the user toggle all pass through SetState.
    if (s != kOn) HideEyeball();
    UpdateTray();
    if (s == kOn) SetTimer(g.hwnd, IDT_METER, 100, nullptr);
    else          KillTimer(g.hwnd, IDT_METER);
}

// ---------------------------------------------------------------- about dialog

static std::wstring AboutText()
{
    // The versions the loaded engine itself reports, not a compile-time constant.
    auto widen = [](const char* s) {
        std::wstring w;
        if (s) { int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); w.resize(n ? n - 1 : 0);
                 MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n); }
        return w; };
    const std::wstring sherpaVer = widen(SherpaOnnxGetVersionStr());
    const std::wstring ortVer    = widen(SherpaOnnxGetOnnxruntimeVersionStr());
    wchar_t buf[1600];
    swprintf_s(buf,
        L"%s %s (%s)\r\n\r\n"
        L"Author:     Piyush Raizada\r\n"
        L"Vibe coded with Hermes Agent (Nous Research), Qwen/Qwen3.8-Flash-Next\r\n"
        L"and deepseek-ai/DeepSeek-V4.1-Flash-1017\r\n\r\n"
        L"Log file:\r\n%s\r\n\r\n"
        L"Speech model:       NVIDIA Nemotron \u2014 NVIDIA Open Model License\r\n"
        L"Punctuation model:  Horizon Labs \u2014 Apache License 2.0 (optional download)\r\n"
        L"Speech engine:      sherpa-onnx %s \u2014 Apache License 2.0\r\n"
        L"Inference runtime:  ONNX Runtime %s \u2014 MIT License (statically linked)\r\n\r\n"
        L"Full licence texts in LICENSE.txt and NOTICE.txt beside the executable.",
        kAppName, kAppVersion, TIRO_GIT_HASH,
        LogPath().c_str(),
        sherpaVer.c_str(), ortVer.c_str());
    return buf;
}

static void CopyToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) { memcpy(p, text.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    }
    CloseClipboard();
}

static INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    UNREFERENCED_PARAMETER(lp);
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_ABOUT_TEXT, AboutText().c_str());
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_COPY) {
            CopyToClipboard(dlg, AboutText());
            SetDlgItemTextW(dlg, IDC_COPY, L"Copied");
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- hotkey dialog

static std::wstring KeyName(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F12) {
        wchar_t b[8];
        swprintf_s(b, L"F%u", vk - VK_F1 + 1);
        return b;
    }
    switch (vk) {
    case VK_SPACE:  return L"Space";
    case VK_RETURN: return L"Enter";
    case VK_TAB:    return L"Tab";
    case VK_ESCAPE: return L"Esc";
    case VK_LEFT:   return L"Left";
    case VK_RIGHT:  return L"Right";
    case VK_UP:     return L"Up";
    case VK_DOWN:   return L"Down";
    case VK_OEM_3:  return L"`";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PLUS:  return L"=";
    default: break;
    }
    return L"?";
}

static std::wstring ComboText(UINT mods, UINT vk)
{
    if (!vk) return L"(none)";
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT)     s += L"Alt+";
    if (mods & MOD_SHIFT)   s += L"Shift+";
    if (mods & MOD_WIN)     s += L"Win+";
    s += KeyName(vk);
    return s;
}

struct HotkeyCtx { UINT mods = 0, vk = 0; };

// Records a captured combination into the dialog. Shared by the capture control.
static void ApplyCapture(HWND dlg, HotkeyCtx* ctx, UINT vk)
{
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
    if (GetKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;

    ctx->mods = mods;
    ctx->vk = vk;
    SetDlgItemTextW(dlg, IDC_HOTKEY_CAPTURE, ComboText(mods, vk).c_str());
    SetDlgItemTextW(dlg, IDC_HOTKEY_ERROR,
                    mods ? L"Press OK to apply this shortcut."
                         : L"A shortcut needs at least one modifier.");
}

// The keystrokes land on this control, so it must swallow them - otherwise the system
// beeps on every key and nothing is captured.
static LRESULT CALLBACK CaptureProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref)
{
    UNREFERENCED_PARAMETER(id);
    auto* ctx = (HotkeyCtx*)ref;
    HWND dlg = GetParent(hwnd);

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
            vk == VK_LWIN || vk == VK_RWIN)
            return 0;
        if (vk == VK_ESCAPE) {
            PostMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
        ApplyCapture(dlg, ctx, vk);
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK HotkeyProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* ctx = (HotkeyCtx*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG:
        ctx = (HotkeyCtx*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)ctx);
        SetDlgItemTextW(dlg, IDC_HOTKEY_CURRENT, ComboText(g.hkMods, g.hkVk).c_str());
        SetDlgItemTextW(dlg, IDC_HOTKEY_CAPTURE, L"(waiting)");
        SetWindowSubclass(GetDlgItem(dlg, IDC_HOTKEY_CAPTURE), CaptureProc, 1, (DWORD_PTR)ctx);
        // Release the global hotkey, otherwise the shell swallows the very combination the user is
        // trying to type - and toggles dictation behind the dialog while they do it.
        UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
        SetFocus(GetDlgItem(dlg, IDC_HOTKEY_CAPTURE));
        return FALSE;

    case WM_DESTROY:
        RemoveWindowSubclass(GetDlgItem(dlg, IDC_HOTKEY_CAPTURE), CaptureProc, 1);
        UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
        if (g.hkVk) RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g.hkMods | MOD_NOREPEAT, g.hkVk);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (!ctx->vk) {
                SetDlgItemTextW(dlg, IDC_HOTKEY_ERROR, L"Press a combination first.");
                return TRUE;
            }
            if (!ctx->mods) {
                SetDlgItemTextW(dlg, IDC_HOTKEY_ERROR, L"A shortcut needs at least one modifier.");
                return TRUE;
            }
            if (!RegisterHotKey(g.hwnd, HOTKEY_TEST, ctx->mods | MOD_NOREPEAT, ctx->vk)) {
                SetDlgItemTextW(dlg, IDC_HOTKEY_ERROR, L"That shortcut is already in use.");
                return TRUE;
            }
            UnregisterHotKey(g.hwnd, HOTKEY_TEST);
            UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
            if (RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, ctx->mods | MOD_NOREPEAT, ctx->vk)) {
                g.hkMods = ctx->mods;
                g.hkVk = ctx->vk;
                RegSetDword(L"HotkeyMods", g.hkMods);
                RegSetDword(L"HotkeyVk", g.hkVk);
                Log(L"INFO", L"hotkey changed to " + ComboText(g.hkMods, g.hkVk));
            }
            EndDialog(dlg, IDOK);
            return TRUE;

        case IDC_CLEAR:
            UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
            g.hkMods = 0;
            g.hkVk = 0;
            RegSetDword(L"HotkeyMods", 0);
            RegSetDword(L"HotkeyVk", 0);
            Log(L"INFO", L"hotkey cleared");
            EndDialog(dlg, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- commands

static void PolishLog(const wchar_t* m) { Log(L"INFO", m); }

static DWORD WINAPI PolishInitThread(LPVOID)
{
    int t = (int)RegGetDword(L"PolishThreads", 1);
    if (!punct::Init(ExeDir() + L"\\models\\punctuation", PolishLog, t)) {
        Log(L"INFO", L"polish disabled: models\\punctuation missing or unreadable");
        // The polish model does not ship in the zip, so this is the
        // DEFAULT install state. A user who opted in must hear why nothing changed
        // instead of watching the rewrite silently never happen.
        if (g.hwnd) PostMessageW(g.hwnd, WM_APP_POLISH_MISSING, 0, 0);
    }
    return 0;
}

// ---- Span selection helper ---------------------------------------------------
static HWND FocusedEditOf(HWND target)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    if (fg != target && !IsChild(target, fg)) return nullptr;   // focus left
    DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gi = { sizeof(gi) };
    if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) return gi.hwndFocus;
    return fg;
}

// ---- One-shot replace for Win32 EDIT / RichEdit -----------------------------
// EM_EXSETSEL + EM_REPLACESEL replaces the dictated span inside the control's
// own thread in two back-to-back messages: no clipboard borrow, no key
// injection. Because the set-select and replace are queued together, the
// control never has to paint the intermediate selection - the user sees raw
// text become polished text in one update.

// ---- Span read-back ---------------------------------------------------------
// Reads exactly [cp - n, cp) back out of the control, so a rewrite can prove the span it
// is about to replace is still the text Tiro typed. Each class gets the message that
// actually works on it - measured, not assumed: a RichEdit counts a line break as ONE
// character position but returns CRLF from WM_GETTEXT, so a whole-document read is
// offset by one per line break (EM_GETTEXTRANGE returns the exact range); a classic
// EDIT keeps CRLF in its buffer, lines up with EM_GETSEL, and does not answer
// EM_GETTEXTRANGE at all.
static bool ReadSpanBack(HWND ed, bool rich, LONG cpMin, size_t n, std::wstring& out)
{
    if (!n || n > 1000000u) return false;                 // nothing sane to verify
    out.assign(n, L'\0');
    if (rich) {
        TEXTRANGE tr{};
        tr.chrg.cpMin = cpMin - (LONG)n;
        tr.chrg.cpMax = cpMin;
        tr.lpstrText  = out.data();
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(ed, EM_GETTEXTRANGE, 0, (LPARAM)&tr, SMTO_ABORTIFHUNG, 300, &copied))
            return false;
        return copied == (DWORD_PTR)n;
    }
    DWORD_PTR len = 0;
    if (!SendMessageTimeoutW(ed, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 200, &len)) return false;
    if (len > 2000000u || (DWORD_PTR)cpMin > len) return false;
    std::wstring all((size_t)len + 1, L'\0');
    DWORD_PTR got = 0;
    if (!SendMessageTimeoutW(ed, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)all.data(),
                             SMTO_ABORTIFHUNG, 500, &got))
        return false;
    if (got < (DWORD_PTR)cpMin) return false;
    out.assign(all, (size_t)cpMin - n, n);
    return true;
}

// ---- One-shot replace, Win32 EDIT / RichEdit --------------------------------
// Returns false for any control that is not a classic edit window, for a live
// foreign selection, when the caret has moved away from the dictated span, or when
// the target is hung; the text is then left as typed.
static bool RewriteViaReplaceSel(HWND target, const std::wstring& raw, const std::wstring& polished)
{
    const size_t nchars = raw.size();
    if (g_polishCancel) return false;
    if (!IsWindow(target) || raw.empty()) return false;
    HWND ed = FocusedEditOf(target);
    if (!ed) return false;
    wchar_t cls[40] = {};
    if (!GetClassNameW(ed, cls, 40)) return false;
    for (size_t i = 0; cls[i]; ++i)
        if (cls[i] >= L'A' && cls[i] <= L'Z') cls[i] = wchar_t(cls[i] - L'A' + L'a');
    // RichEditD2DPT: the D2D-rendered editor hosting Windows 11's Store Notepad.
    // It passes class checks like a RichEdit but refuses the replace messages -
    // and corrupts injected keystrokes outright. Never rewrite through it.
    if (wcsstr(cls, L"d2dpt")) return false;
    const bool rich = wcsstr(cls, L"richedit") != nullptr;
    const bool edit = !rich && wcscmp(cls, L"edit") == 0;
    if (!rich && !edit) return false;
    if (rich) {
        CHARRANGE cr = {};
        if (!SendMessageTimeoutW(ed, EM_EXGETSEL, 0, (LPARAM)&cr, SMTO_ABORTIFHUNG, 200, nullptr)) return false;
        if (cr.cpMin != cr.cpMax || cr.cpMin < 0 || (size_t)cr.cpMin < nchars) return false;
        // The caret must sit where the dictation left it AND the characters behind it
        // must still be the ones Tiro typed. Without this second test a click into
        // another part of the same document during the rewrite would make the
        // "dictated span" somebody else's paragraph.
        std::wstring seen;
        if (!ReadSpanBack(ed, true, cr.cpMin, nchars, seen) || seen != raw) {
            Log(L"WARN", L"polish: the span behind the caret is not the dictated text; left as typed");
            return false;
        }
        CHARRANGE sel = { cr.cpMin - (LONG)nchars, cr.cpMin };
        DWORD_PTR r = 0;
        // Suspend redraw so the set-select never paints: the control draws once,
        // showing the polished text. Redraw is always re-enabled below.
        SendMessageTimeoutW(ed, WM_SETREDRAW, FALSE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
        BOOL setOk = (SendMessageTimeoutW(ed, EM_EXSETSEL, 0, (LPARAM)&sel, SMTO_ABORTIFHUNG, 200, &r) != 0) && r;
        BOOL repOk = FALSE;
        if (setOk)
            repOk = SendMessageTimeoutW(ed, EM_REPLACESEL, FALSE, (LPARAM)polished.c_str(),
                                        SMTO_ABORTIFHUNG, 1000, nullptr) != 0;
        SendMessageTimeoutW(ed, WM_SETREDRAW, TRUE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
        RedrawWindow(ed, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return repOk != FALSE;
    }
    DWORD sel = 0;
    if (!SendMessageTimeoutW(ed, EM_GETSEL, 0, (LPARAM)&sel, SMTO_ABORTIFHUNG, 200, nullptr)) return false;
    DWORD start = LOWORD(sel), end = HIWORD(sel);
    if (start != end || (size_t)start < nchars) return false;
    std::wstring seen;
    if (!ReadSpanBack(ed, false, (LONG)start, nchars, seen) || seen != raw) {
        Log(L"WARN", L"polish: the span behind the caret is not the dictated text; left as typed");
        return false;
    }
    SendMessageTimeoutW(ed, WM_SETREDRAW, FALSE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
    BOOL setOk = SendMessageTimeoutW(ed, EM_SETSEL, start - (UINT)nchars, (LPARAM)start, SMTO_ABORTIFHUNG, 200, nullptr) != 0;
    BOOL repOk = FALSE;
    if (setOk)
        repOk = SendMessageTimeoutW(ed, EM_REPLACESEL, FALSE, (LPARAM)polished.c_str(),
                                    SMTO_ABORTIFHUNG, 1000, nullptr) != 0;
    SendMessageTimeoutW(ed, WM_SETREDRAW, TRUE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
    RedrawWindow(ed, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return repOk != FALSE;
}

// ---- One-shot replace via UIA ValuePattern (Chromium / Electron / WinUI) -----
// Writable inputs that are not classic edit windows still accept a whole-value
// SetValue through UI Automation: read the current value, splice the polished
// span over the raw span, write the whole string back in one call. No clipboard,
// no keystrokes, no painted selection. Safe by construction: the write happens
// only after the raw span is found exactly where the caret left it; anything
// unexpected (user typed on, provider reports a different value) aborts and the
// text is left exactly as injected.
static bool RewriteViaUIAValue(HWND target, const std::wstring& raw, const std::wstring& polished)
{
    if (g_polishCancel) return false;
    if (!IsWindow(target) || raw.empty()) return false;
    HWND fg = GetForegroundWindow();
    if (fg != target && !IsChild(target, fg)) return false;

    static IUIAutomation* uia = nullptr;
    static bool uiaTried = false;
    if (!uiaTried) {
        uiaTried = true;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(ci) || ci == RPC_E_CHANGED_MODE)
            CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&uia));
    }
    if (!uia) return false;

    bool ok = false;
    IUIAutomationElement* el = nullptr;
    IUnknown* pVal = nullptr; IUnknown* pTxt = nullptr;
    IUIAutomationValuePattern* vp = nullptr;
    IUIAutomationTextPattern* tp = nullptr;
    IUIAutomationTextRangeArray* arr = nullptr;
    IUIAutomationTextRange* sel = nullptr;
    IUIAutomationTextRange* doc = nullptr;
    IUIAutomationTextRange* pre = nullptr;
    IUIAutomationTextRange* post = nullptr;
    BSTR bstr = nullptr;
    do {
        if (FAILED(uia->GetFocusedElement(&el)) || !el) break;
        UIA_HWND hh = 0;
        if (SUCCEEDED(el->get_CurrentNativeWindowHandle(&hh)) && hh) {
            HWND eh = (HWND)(INT_PTR)hh;
            if (eh != target && !IsChild(target, eh) && !IsChild(eh, target)) break;
        }
        if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &pVal)) || !pVal) break;
        if (FAILED(pVal->QueryInterface(IID_PPV_ARGS(&vp)))) break;
        BOOL ro = TRUE;
        if (FAILED(vp->get_CurrentIsReadOnly(&ro)) || ro) break;
        if (FAILED(el->GetCurrentPattern(UIA_TextPatternId, &pTxt)) || !pTxt) break;
        if (FAILED(pTxt->QueryInterface(IID_PPV_ARGS(&tp)))) break;

        // Locate the dictated span: UIA selection must equal the raw text.
        if (FAILED(tp->GetSelection(&arr)) || !arr) break;
        int n = 0;
        if (FAILED(arr->get_Length(&n)) || n != 1) break;
        // Caret gives an empty selection: fall back to the point right before it.
        if (FAILED(arr->GetElement(0, &sel)) || !sel) break;
        if (FAILED(sel->GetText(-1, &bstr))) break;
        std::wstring selText = bstr ? bstr : L"";
        SysFreeString(bstr); bstr = nullptr;
        if (selText != raw) {
            // Providers usually report the caret as a collapsed range; rebuild the
            // span by walking back raw.size() characters from the caret.
            IUIAutomationTextRange* caret = nullptr;
            if (FAILED(sel->Clone(&caret)) || !caret) break;
            int moved = 0;
            if (FAILED(caret->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                                                 -(int)raw.size(), &moved)) || moved != -(int)raw.size()) {
                caret->Release(); break;
            }
            sel->Release();
            sel = caret;
            if (FAILED(sel->GetText(-1, &bstr))) break;
            selText = bstr ? bstr : L"";
            SysFreeString(bstr); bstr = nullptr;
            if (selText != raw) break;   // user moved on; leave it alone
        }

        // Splice: value must be exactly [before][raw][after].
        if (FAILED(tp->get_DocumentRange(&doc)) || !doc) break;
        IUIAutomationTextRange* preR = nullptr; IUIAutomationTextRange* postR = nullptr;
        if (FAILED(doc->Clone(&preR)) || FAILED(doc->Clone(&postR))) {
            if (preR) preR->Release(); if (postR) postR->Release(); break;
        }
        pre = preR; post = postR;
        if (FAILED(pre->MoveEndpointByRange(TextPatternRangeEndpoint_End, sel, TextPatternRangeEndpoint_Start))) break;
        if (FAILED(post->MoveEndpointByRange(TextPatternRangeEndpoint_Start, sel, TextPatternRangeEndpoint_End))) break;
        if (FAILED(pre->GetText(-1, &bstr))) break;
        std::wstring before = bstr ? bstr : L""; SysFreeString(bstr); bstr = nullptr;
        if (FAILED(post->GetText(-1, &bstr))) break;
        std::wstring after = bstr ? bstr : L""; SysFreeString(bstr); bstr = nullptr;
        if (FAILED(vp->get_CurrentValue(&bstr))) break;
        std::wstring cur = bstr ? bstr : L""; SysFreeString(bstr); bstr = nullptr;
        if (cur != before + raw + after) break;   // provider disagrees with the text model; abort

        std::wstring nv = before + polished + after;
        BSTR nvB = SysAllocString(nv.c_str());
        HRESULT setHr = nvB ? vp->SetValue(nvB) : E_OUTOFMEMORY;
        if (nvB) SysFreeString(nvB);
        if (FAILED(setHr)) break;

        // Whole-value writes leave the caret wherever the provider likes, and the
        // accessibility tree refreshes asynchronously. Poll the fresh document
        // range until it reports the new value, then walk back over the untouched
        // tail and select the zero-length range: caret lands right after the
        // polished span so dictation can continue from there.
        // The write itself is not the proof: some providers (Antigravity's chat
        // box) accept SetValue and never apply it. Success means the text model
        // reported the new value; anything else is a refusal, reported as one.
        bool settled = false;
        ULONGLONG caretT0 = GetTickCount64();
        for (int attempt = 0; GetTickCount64() - caretT0 < 1000; ++attempt) {
            IUIAutomationTextRange* doc2 = nullptr;
            if (FAILED(tp->get_DocumentRange(&doc2)) || !doc2) break;
            BSTR dv = nullptr;
            std::wstring docTxt;
            if (SUCCEEDED(doc2->GetText(-1, &dv))) docTxt = dv ? dv : L"";
            if (dv) SysFreeString(dv);
            settled = (docTxt == nv);
            if (settled) {
                IUIAutomationTextRange* caret = nullptr;
                if (SUCCEEDED(doc2->Clone(&caret)) && caret) {
                    caret->MoveEndpointByRange(TextPatternRangeEndpoint_Start, doc2,
                                               TextPatternRangeEndpoint_End);
                    int mv = 0;
                    caret->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                                              -(int)after.size(), &mv);
                    caret->MoveEndpointByRange(TextPatternRangeEndpoint_End, caret,
                                               TextPatternRangeEndpoint_Start);
                    if (mv == -(int)after.size()) caret->Select();
                    caret->Release();
                }
                doc2->Release();
                break;
            }
            doc2->Release();
            Sleep(40);
        }
        if (!settled) Log(L"WARN", L"polish: uia-value write was accepted but never appeared; treating as refusal");
        ok = settled;
    } while (false);
    if (bstr) SysFreeString(bstr);
    if (pre) pre->Release();
    if (post) post->Release();
    if (doc) doc->Release();
    if (sel) sel->Release();
    if (arr) arr->Release();
    if (tp) tp->Release();
    if (pTxt) pTxt->Release();
    if (vp) vp->Release();
    if (pVal) pVal->Release();
    if (el) el->Release();
    return ok;
}

// ---- Capability: can this target take a one-shot punctuation rewrite? -------
// Two accepted shapes: a classic EDIT/RichEdit child (EM_REPLACESEL path) or a
// focused UIA element exposing a writable ValuePattern plus a TextPattern (the
// SetValue path; TextPattern is required so the rewrite can verify the span
// before touching anything, and so list items or buttons never qualify).
static IUIAutomation* UiaSingleton()
{
    static IUIAutomation* uia = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(ci) || ci == RPC_E_CHANGED_MODE)
            CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&uia));
    }
    return uia;
}

static bool FocusedUiaValueWritable(HWND target)
{
    IUIAutomation* uia = UiaSingleton();
    if (!uia) return false;
    IUIAutomationElement* el = nullptr;
    IUnknown* pVal = nullptr; IUnknown* pTxt = nullptr;
    IUIAutomationValuePattern* vp = nullptr;
    IUIAutomationTextPattern* tp = nullptr;
    bool ok = false;
    do {
        if (FAILED(uia->GetFocusedElement(&el)) || !el) break;
        UIA_HWND hh = 0;
        if (SUCCEEDED(el->get_CurrentNativeWindowHandle(&hh)) && hh) {
            HWND eh = (HWND)(INT_PTR)hh;
            if (eh != target && !IsChild(target, eh) && !IsChild(eh, target)) break;
        }
        if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &pVal)) || !pVal) break;
        if (FAILED(pVal->QueryInterface(IID_PPV_ARGS(&vp)))) break;
        BOOL ro = TRUE;
        if (FAILED(vp->get_CurrentIsReadOnly(&ro)) || ro) break;
        // A text surface is required, not just a settable value.
        if (FAILED(el->GetCurrentPattern(UIA_TextPatternId, &pTxt)) || !pTxt) break;
        if (FAILED(pTxt->QueryInterface(IID_PPV_ARGS(&tp)))) break;
        ok = true;
    } while (false);
    if (tp) tp->Release();
    if (pTxt) pTxt->Release();
    if (vp) vp->Release();
    if (pVal) pVal->Release();
    if (el) el->Release();
    return ok;
}

// UIA calls into a misbehaving provider can block for seconds, so the query runs
// on its own thread and TurnOn waits at most 700 ms. Unknown falls back to
// "unsupported", which is the conservative answer: the speech model keeps
// typing its own punctuation and no rewrite will be attempted.
struct CapProbe { HWND top; bool ok; HANDLE done; };
static DWORD WINAPI CapProbeThread(LPVOID p)
{
    CapProbe* c = (CapProbe*)p;
    c->ok = FocusedUiaValueWritable(c->top);
    SetEvent(c->done);
    return 0;
}

static bool TargetSupportsOneShot(HWND top)
{
    if (!top || !IsWindow(top)) return false;
    HWND fe = FocusedEditOf(top);
    if (fe) {
        wchar_t cls[32]{};
        GetClassNameW(fe, cls, 32);
        // See RewriteViaReplaceSel: D2DPT hosts answer class checks but refuse
        // real work; declare them unsupported up front so the session keeps the
        // speech model's own punctuation and the closing-period rule.
        if (wcsstr(cls, L"D2DPT") || wcsstr(cls, L"d2dpt")) return false;
        if (_wcsicmp(cls, L"Edit") == 0 || wcsstr(cls, L"RICHEDIT")) return true;
    }
    CapProbe probe{ top, false, CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    if (!probe.done) return false;
    HANDLE th = CreateThread(nullptr, 0, CapProbeThread, &probe, 0, nullptr);
    bool ok = false;
    if (th) {
        ok = (WaitForSingleObject(probe.done, 700) == WAIT_OBJECT_0) && probe.ok;
        if (!ok) CloseHandle(th);   // hung provider: the thread finishes on its own
    }
    CloseHandle(probe.done);
    return ok;
}

// The mode decision runs here so TurnOn returns to opening the microphone
// immediately. A result is only allowed to land for the session that started
// this probe; a stale probe from a superseded session logs and withdraws.
static DWORD WINAPI ModeProbeThread(LPVOID p)
{
    ModeProbeCtx* ctx = (ModeProbeCtx*)p;
    bool ok = g_polishEnabled && TargetSupportsOneShot(ctx->target);
    if (g_sessionGen == ctx->gen) {
        g_sessionPolish = ok;
        g_probePending = false;
    }
    delete ctx;
    Log(L"INFO", ok
        ? L"polish mode: punctuation model (target supports one-shot replace)"
        : L"polish mode: speech-model punctuation (target does not support one-shot replace)");
    return 0;
}

struct PolishJob { HWND target; std::wstring text; };

static DWORD WINAPI PolishThread(LPVOID p)
{
    auto* job = (PolishJob*)p;
    g_polishCancel = false;
    std::wstring out;
    ULONGLONG t0 = GetTickCount64();
    try { out = punct::Polish(job->text); } catch (...) { out.clear(); }
    if (!out.empty() && !job->text.empty()) {
        // The model trims; whitespace the user actually dictated at the span
        // edges (typically the leading space that joins this session to the
        // previous one) stays visible - the rewrite replaces those characters,
        // so it must re-emit them.
        if (iswspace(job->text.front()) && !iswspace(out.front()))
            out.insert(out.begin(), L' ');
        if (iswspace(job->text.back()) && !iswspace(out.back()))
            out.push_back(L' ');
    }
    ULONGLONG dt = GetTickCount64() - t0;

    wchar_t b[192];
    if (g_polishCancel || g.state == kOn) {
        Log(L"INFO", L"polish: cancelled by a new dictation session");
        delete job; return 0;
    }
    if (out.empty()) {
        Log(L"INFO", L"polish: model failed, text left as typed");
        delete job; return 0;
    }
    if (out == job->text) { delete job; return 0; }   // nothing changed, nothing to retype
    HWND fg = GetForegroundWindow();
    if (!job->target || !IsWindow(job->target) || (fg != job->target && !IsChild(job->target, fg))) {
        Log(L"INFO", L"polish: target window is no longer foreground, text left as typed");
        delete job; return 0;
    }
    // Two one-shot paths, no keystrokes and no clipboard anywhere:
    // EM_REPLACESEL for classic edit/RichEdit controls; UIA ValuePattern.SetValue
    // for Chromium/Electron/WinUI inputs that expose a writable value. If neither
    // applies the text is left exactly as typed and the log says so.
    ULONGLONG t1 = GetTickCount64();
    bool oneShot = RewriteViaReplaceSel(job->target, job->text, out);
    const wchar_t* via = L"replace-sel";
    if (!oneShot) {
        oneShot = RewriteViaUIAValue(job->target, job->text, out);
        via = L"uia-value";
    }
    ULONGLONG dt2 = GetTickCount64() - t1;
    if (LogTextEnabled()) {
        // Raw in / polished out under the same diagnostic switch as the inject
        // lines: app.log carries words only when the user opts into LogText.
        Log(L"INFO", (L"polish raw: " + job->text).c_str());
        if (oneShot) Log(L"INFO", (L"polish out: " + out).c_str());
    }
    if (oneShot)
        swprintf_s(b, L"polish: %zu chars -> %zu via %ls in %llu ms (model %llu ms)",
                   job->text.size(), out.size(), via,
                   (unsigned long long)dt2, (unsigned long long)dt);
    else
        swprintf_s(b, L"polish: target does not support one-shot replace; text left as typed (model %llu ms)",
                   (unsigned long long)dt);
    Log(L"WARN", b);
    // The rewrite was the closing writer for this mode and it just failed.
    // The text keeps every mark the speech model supplied - nothing is ever
    // stripped - but the model's final period is exactly what streaming
    // drops, so repay that one mark from own pocket: type it at the caret
    // (which the failed rewrite never moved). Re-open the drain window around
    // it, since the injector refuses text once dictation has fully closed.
    // If the model did supply a trailing mark, this stays silent.
    if (g_uttStarted) {
        wchar_t last = 0;
        { std::lock_guard<std::mutex> il(g_injectMu); last = g_lastInjected; }
        if (iswalnum(last)) {
            g_drainActive = true;
            QueueInject(L".", false);
            Log(L"INFO", L"polish fallback: closing period typed instead");
            ULONGLONG pl = GetTickCount64() + 800;
            for (;;) {
                bool idle = false;
                { std::lock_guard<std::mutex> il(g_injectMu); idle = g_injectQ.empty(); }
                { std::lock_guard<std::mutex> pl2(g_polishMu); if (idle && !g_delivering) break; }
                if (GetTickCount64() > pl) {
                    Log(L"WARN", L"polish fallback: closing period still queued at close");
                    break;
                }
                Sleep(20);
            }
            g_drainActive = false;
        }
    }
    delete job;
    return 0;
}

// Single stop path. Anything that ends dictation — the user, device loss, focus loss — goes
// through here so the teardown order is identical.
static void StopDictation(const wchar_t* why, bool userInitiated = false)
{
    if (g.state != kOn) return;
    HideEyeball();
    if (userInitiated)
    {
        // When the USER stops, the words still in flight are the end of their
        // sentence, not stale speech. Close the microphone and let the inference thread decode
        // what it already has; the injector types it (drainActive), the drain completes only
        // when the document has the words, and only then does the polish pass see the session.
        // Bounded by drainUntil so a slow decoder can never type long after the click.
        CloseCapture();
        if (!g_engine.active) {      // engine already gone: nothing will ever drain, so drop
            ClearInjectQueue();
            DropPendingAudio();
            SetState(kOff);
            Log(L"INFO", std::wstring(L"dictation off: ") + why);
            return;
        }
        g_drainActive = true;        // injector accepts the tail while the app is already Off
        {
            std::lock_guard<std::mutex> lk(g_engine.mu);
            g_engine.drain = true;
            g_engine.drainPolish = g_sessionPolish;
            g_engine.drainUntil = GetTickCount64() + 2000;
        }
        g_engine.cv.notify_all();
        SetState(kOff);
        Log(L"INFO", std::wstring(L"dictation off (draining tail): ") + why);
        return;
    }
    ClearInjectQueue();
    DropPendingAudio();
    CloseCapture();
    SetState(kOff);
    Log(L"INFO", std::wstring(L"dictation off: ") + why);
}

static void Toggle()
{
    if (g.state == kLoading) {
        Log(L"INFO", L"toggle ignored while loading");
        return;
    }
    if (g.state == kOn) {
        StopDictation(L"capture closed by the user", true);
    } else {
        // The click that took focus from the document also arrives here as a tray click. Without
        // this the guard stops dictation and the same click immediately turns it back on.
        if (GetTickCount64() - g.autoStopAt < 500) {
            Log(L"INFO", L"toggle ignored: it is the click that just ended dictation");
            return;
        }
        TurnOn();
    }
}

static bool StartupShortcutPath(std::wstring& out)
{
    wchar_t dir[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, dir))) return false;
    out = std::wstring(dir) + L"\\Tiro.lnk";
    return true;
}

// Per-user autostart, the honest way: one .lnk to this exe in the user's
// Startup folder (roaming profile). No registry Run key, no scripts, no admin.
static void StartupToggle()
{
    std::wstring lnk;
    if (!StartupShortcutPath(lnk)) {
        Log(L"WARN", L"startup: cannot resolve the user Startup folder");
        Notify(L"Could not find your Startup folder.");
        return;
    }
    if (GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (DeleteFileW(lnk.c_str()))
            Log(L"INFO", L"startup: shortcut removed - " + lnk);
        else
            Log(L"WARN", L"startup: could not remove shortcut - " + lnk);
        return;
    }
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(g.hInst, exe, MAX_PATH);   // wherever this copy lives
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        IShellLinkW* link = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_IShellLinkW, (void**)&link))) {
            link->SetPath(exe);
            link->SetWorkingDirectory(exe);       // so models\ beside the exe resolves
            link->SetDescription(L"Tiro dictation");
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&pf))) {
                hr = pf->Save(lnk.c_str(), TRUE);
                pf->Release();
            }
            link->Release();
        }
        CoUninitialize();
    }
    if (SUCCEEDED(hr))
        Log(L"INFO", L"startup: shortcut created - " + lnk);
    else {
        Log(L"WARN", L"startup: could not create shortcut");
        Notify(L"Could not create the Startup shortcut.");
    }
}

// Restart without scripting: hand a fresh copy of ourselves an inherited
// handle to OUR process object - the kernel signals it exactly when we exit,
// so the child takes the tray slot (and the single-instance mutex) cleanly.
static void RelaunchAndExit()
{
    if (g.state == kOn) SetState(kOff);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(g.hInst, exe, MAX_PATH);
    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                         &self, SYNCHRONIZE, TRUE, 0)) {
        Log(L"WARN", L"restart: could not duplicate process handle");
        Notify(L"Could not restart.");
        return;
    }
    wchar_t cmd[MAX_PATH + 64]{};
    swprintf_s(cmd, L"\"%s\" --wait-handle %llu", exe,
               (unsigned long long)(uintptr_t)self);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // bInheritHandles=TRUE carries `self` to the child; safe because the
    // mic/event handles are private to threads that die with us.
    BOOL ok = CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                             DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(self);
    if (!ok) {
        Log(L"WARN", L"restart: could not launch the new instance");
        Notify(L"Could not restart.");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log(L"INFO", L"restart: new instance launched; exiting");
    DestroyWindow(g.hwnd);
}

static void HandleCommand(UINT cmd)
{
    if (cmd == IDM_RESTART) { RelaunchAndExit(); return; }
    if (cmd == IDM_STARTUP) { StartupToggle(); return; }
    if (cmd == IDM_EXIT) {
        Log(L"INFO", L"exit requested from tray menu");
        DestroyWindow(g.hwnd);
        return;
    }
    if (cmd == IDM_ABOUT) {
        // Stop through the one stop path: SetState(kOff) alone left the microphone open.
        if (g.state == kOn) StopDictation(L"a dialog was opened");
        DialogBoxW(g.hInst, MAKEINTRESOURCEW(IDD_ABOUT), g.hwnd, AboutProc);
        return;
    }
    if (cmd == IDM_HOTKEY) {
        if (g.state == kOn) StopDictation(L"a dialog was opened");
        HotkeyCtx ctx{};
        DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_HOTKEY), g.hwnd, HotkeyProc, (LPARAM)&ctx);
        return;
    }
    if (cmd == IDM_EYE) {
        bool show = RegGetDword(L"ShowEye", 1) != 0;
        RegSetDword(L"ShowEye", show ? 0 : 1);
        if (show) HideEyeball();   // switching off mid-dictation: gone right now
        Log(L"INFO", show ? L"eyeball animation disabled from tray menu"
                          : L"eyeball animation enabled from tray menu");
        return;
    }
    if (cmd == IDM_POLISH) {
        // Restart-only: toggling the live flag could kill a rewrite already in
        // flight (disable) or promise polish the model can't deliver (enable, if
        // it was off at startup and never loaded). Record the wish, tell them
        // plainly when it lands; the menu check reads the registry so it sticks.
        const bool want = RegGetDword(L"Polish", 0) == 0;  // new desired state
        RegSetDword(L"Polish", want ? 1 : 0);
        Log(L"INFO", want ? L"punctuation enable requested from tray menu; applies after restart"
                          : L"punctuation disable requested from tray menu; applies after restart");
        Notify(want ? L"Punctuation will be enabled after Tiro is restarted."
                    : L"Punctuation will be disabled after Tiro is restarted.");
        return;
    }
    if (cmd == IDM_MIC_DEFAULT) {
        ApplyDeviceChange(L"", L"system default");
        return;
    }
    if (cmd >= IDM_MIC_FIRST && cmd <= IDM_MIC_LAST) {
        size_t i = cmd - IDM_MIC_FIRST;
        if (i < g.devices.size())
            ApplyDeviceChange(g.devices[i].id, g.devices[i].name);
        return;
    }
    if (cmd >= IDM_MODEL_FIRST && cmd <= IDM_MODEL_LAST) {
        size_t i = cmd - IDM_MODEL_FIRST;
        if (i < g.models.size()) {
            if (g.models[i].folder != g.modelFolder) {
                if (g.state == kOn) StopDictation(L"the model was changed");
                UnloadEngine();
                g.modelFolder = g.models[i].folder;
                RegSetStr(L"ModelFolder", g.modelFolder);
                Log(L"INFO", L"model selected: " + g.models[i].name +
                             L" (loads at the next dictation start)");
            }
        }
        return;
    }
}

static void ShowMenu()
{
    g.devices = EnumDevices();
    g.models  = EnumModels();

    HMENU root = CreatePopupMenu();
    HMENU mic  = CreatePopupMenu();
    HMENU mdl  = CreatePopupMenu();

    if (g.devices.empty()) {
        AppendMenuW(mic, MF_STRING | MF_GRAYED, 0, L"(no microphones)");
    } else {
        AppendMenuW(mic, MF_STRING | (g.deviceId.empty() ? MF_CHECKED : 0), IDM_MIC_DEFAULT,
                    L"(System default)");
        for (size_t i = 0; i < g.devices.size() && i < 90; ++i) {
            UINT flags = MF_STRING | ((g.deviceId == g.devices[i].id) ? MF_CHECKED : 0);
            AppendMenuW(mic, flags, IDM_MIC_FIRST + (UINT)i, g.devices[i].name.c_str());
        }
    }

    if (g.models.empty()) {
        AppendMenuW(mdl, MF_STRING | MF_GRAYED, 0, L"(no models found)");
    } else {
        for (size_t i = 0; i < g.models.size() && i < 90; ++i) {
            bool active = g.modelFolder.empty() ? (i == 0) : (g.models[i].folder == g.modelFolder);
            AppendMenuW(mdl, MF_STRING | (active ? MF_CHECKED : 0), IDM_MODEL_FIRST + (UINT)i,
                        g.models[i].name.c_str());
        }
    }

    UINT enable = (g.state == kLoading) ? MF_GRAYED : 0;
    AppendMenuW(root, MF_POPUP | enable, (UINT_PTR)mic, L"Select Microphone");
    AppendMenuW(root, MF_POPUP | enable, (UINT_PTR)mdl, L"Select Model");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_STRING, IDM_HOTKEY, L"Configure Hotkey\u2026");
    AppendMenuW(root, MF_STRING | (RegGetDword(L"ShowEye", 1) == 0 ? MF_CHECKED : 0),
                IDM_EYE, L"Disable Eyeball Animation");
    AppendMenuW(root, MF_STRING | (RegGetDword(L"Polish", 0) ? 0 : MF_CHECKED),
                IDM_POLISH, L"Disable Punctuation");
    std::wstring startupLnk;
    const bool startupOn = StartupShortcutPath(startupLnk) &&
                           GetFileAttributesW(startupLnk.c_str()) != INVALID_FILE_ATTRIBUTES;
    AppendMenuW(root, MF_STRING | (startupOn ? MF_CHECKED : 0),
                IDM_STARTUP, L"Enable at Startup");
    AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(root, MF_STRING, IDM_RESTART, L"Restart");
    AppendMenuW(root, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(root, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g.hwnd);
    UINT cmd = TrackPopupMenu(root, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(root);

    if (cmd) HandleCommand(cmd);
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { Toggle(); return 0; }
        if (LOWORD(lp) == WM_RBUTTONUP) { ShowMenu(); return 0; }
        return 0;

    case WM_APP_RUNNING:
        Log(L"INFO", L"second instance was refused: already running");
        return 0;

    case WM_APP_TEST_ABOUT:
        if (g.state == kOn) StopDictation(L"a dialog was opened");
        DialogBoxW(g.hInst, MAKEINTRESOURCEW(IDD_ABOUT), g.hwnd, AboutProc);
        if (g_debug) PostQuitMessage(0);      // a diagnostic command ends with its dialog
        return 0;

    case WM_APP_TEST_STARTUP:
        StartupToggle();
        if (g_debug) PostQuitMessage(0);
        return 0;

    case WM_APP_TEST_RESTART:
        RelaunchAndExit();
        return 0;

    case WM_APP_TEST_NOTIFY:
        Notify(L"This is a test notification. It disappears after five seconds.");
        if (g_debug) SetTimer(hwnd, IDT_DEBUGQUIT, 6000, nullptr);   // outlive the popup, then exit
        return 0;

    case WM_APP_TEST_HOTKEY: {
        if (g.state == kOn) StopDictation(L"a dialog was opened");
        HotkeyCtx ctx{};
        DialogBoxParamW(g.hInst, MAKEINTRESOURCEW(IDD_HOTKEY), g.hwnd, HotkeyProc, (LPARAM)&ctx);
        if (g_debug) PostQuitMessage(0);
        return 0;
    }

    case WM_APP_DEVICE_LOST:
        HideEyeball();
        StopEngine();
        CloseCapture();
        SetState(kOff);
        Log(L"WARN", L"microphone lost: capture stopped, dictation off");
        Notify(L"The microphone used for transcription was lost. Dictation stopped.");
        return 0;

    case WM_APP_FOCUS_LOST:
        if (g.state == kOn) {
            wchar_t title[128]{};
            if (g.target) GetWindowTextW(g.target, title, 128);
            g.autoStopAt = GetTickCount64();
            StopDictation(L"the target window lost focus");
            Notify(std::wstring(L"Dictation stopped: \"") + title + L"\" lost focus.");
        }
        return 0;

    case WM_APP_POLISH: {
        HWND target = g.target;
        std::wstring text;
        {   // let a segment that is mid-SendUnicode finish before the text is taken
            std::unique_lock<std::mutex> lk(g_polishMu);
            g_polishCv.wait_for(lk, std::chrono::seconds(3), [] { return !g_delivering; });
            text.swap(g_sessionText);
        }
        if (text.empty() || !punct::Ready()) {
            if (!text.empty()) Log(L"INFO", L"polish skipped: model not ready yet");
            return 0;
        }
        auto* job = new PolishJob{ target, std::move(text) };
        if (!CreateThread(nullptr, 0, PolishThread, job, 0, nullptr)) delete job;
        return 0;
    }

    case WM_APP_POLISH_MISSING:
        // The polish model is an optional download; a user who enabled polish
        // without it gets the reason and the fix, once, instead of silence.
        Notify(L"Punctuation model not found. It is optional and not part of the download \u2014 "
               L"see README.txt to add it, then restart Tiro.");
        return 0;

    case WM_APP_ENGINE_READY:
        Log(L"INFO", L"engine ready");
        if (g.state == kLoading) SetState(kOff);
        return 0;

    case WM_APP_ENGINE_FAIL:
        Log(L"ERROR", L"engine failed to load");
        Notify(L"Transcription engine failed to load. See the log file.");
        if (g.state != kLoading) {     // runtime death: stop and fall back to Off
            StopEngine();
            CloseCapture();
            SetState(kOff);
        }
        return 0;                      // at startup we stay in Loading (pale loading arc), Exit only

    case WM_TIMER:
        if (wp == IDT_DEBUGQUIT) { KillTimer(hwnd, IDT_DEBUGQUIT); PostQuitMessage(0); return 0; }
        if (wp == IDT_WATCH) { WatchForeground(); return 0; }
        if (wp == IDT_EYE) {
            EyeballTick();
            return 0;
        }
        if (wp == IDT_METER) {
            // The status pop-up re-arms itself: once it has faded after silence, the next speech
            // brings it back without touching the toggle. The bar is relative, not a fixed 2%:
            // a boom arm pushed an inch farther muted the signal to peaks 0-6% and whole
            // sentences spoke under the old gate. refRms = loudest recent speech (halves ~34 s);
            // avgRms = ~40 s ambience average so steady hiss can never be the reference. Re-arm
            // at 30% of the ceiling but never within 1.6x of the hiss floor, and never below
            // 0.4% absolute.
            g_eye.refRms = (std::max)(g_eye.refRms * 0.9983, g_audio.rms);
            g_eye.avgRms += (g_audio.rms - g_eye.avgRms) * 0.0025;
            const double rearm = (std::max)({ 0.004, g_eye.avgRms * 1.6, g_eye.refRms * 0.30 });
            if (g.state == kOn && !g_eye.visible && g_audio.rms > rearm)
                ShowEyeball();

            double p = g_audio.peak;
            g_audio.peak = 0.0;
            static int ticks = 0;
            if (++ticks % 20 == 0) {
                wchar_t b[128];
                swprintf_s(b, L"audio: peak=%d%% frames=%llu", (int)(p * 100),
                           (unsigned long long)g_audio.frames);
                Log(L"DEBUG", b);
            }
            return 0;
        }
        if (wp == IDT_LOAD) {
            KillTimer(hwnd, IDT_LOAD);
            DetectCores();
            g.models = EnumModels();
            Log(L"INFO", std::wstring(L"core map: ") + std::to_wstring(g.pcores) + L" P-cores of "
                         + std::to_wstring(g.allcores) + L", " + std::to_wstring(g.models.size()) + L" model(s)");
            StartEngine();     // Loading ends when the engine reports ready
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE) { Toggle(); return 0; }
        return 0;

    case WM_CREATE: {
        g.wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        int cx = GetSystemMetrics(SM_CXSMICON);
        g.icon[kLoading] = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCEW(IDI_LOADING), IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
        g.icon[kOff]     = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCEW(IDI_OFF),     IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
        g.icon[kOn]      = (HICON)LoadImageW(g.hInst, MAKEINTRESOURCEW(IDI_ON),      IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
        Log(L"DEBUG", L"icons loaded: loading=" + std::to_wstring((INT_PTR)g.icon[kLoading]) +
                      L" off=" + std::to_wstring((INT_PTR)g.icon[kOff]) +
                      L" on=" + std::to_wstring((INT_PTR)g.icon[kOn]) +
                      L" size=" + std::to_wstring(cx));
        return 0;
    }

    case WM_DESTROY:
        if (g.trayAdded) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            g.trayAdded = false;
        }
        PostQuitMessage(0);
        return 0;
    }

    if (msg == g.wmTaskbarCreated && g.wmTaskbarCreated) {
        g.trayAdded = false;
        UpdateTray();
        if (g.hkVk) RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g.hkMods | MOD_NOREPEAT, g.hkVk);
        Log(L"INFO", L"explorer restarted: tray icon re-added");
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    g.hInst = hInst;
    g_debug = HasSwitch(L"--debug");               // the one switch that unlocks diagnostics
    const bool debug = g_debug;
    RotateLogIfLarge();

    // Restart handoff: wait for the launching instance to die before we even
    // look at the mutex, so the tray slot and the mutex transfer cleanly.
    {
        int n = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 1; argv && i < n; ++i) {
            if (!_wcsicmp(argv[i], L"--wait-handle") && i + 1 < n) {
                HANDLE h = (HANDLE)(uintptr_t)_wcstoui64(argv[i + 1], nullptr, 10);
                // The number is a claim from the command line, not a fact: anything this
                // process does not already hold is refused instead of waited on and closed.
                DWORD info = 0;
                if (h && h != INVALID_HANDLE_VALUE && GetHandleInformation(h, &info)) {
                    WaitForSingleObject(h, 15000);   // cap it: a wedged parent must not wed us
                    CloseHandle(h);
                } else if (h) {
                    Log(L"WARN", L"restart: --wait-handle is not a handle of this process; ignored");
                }
            }
        }
        if (argv) LocalFree(argv);
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Tiro.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!debug) {
            HWND other = FindWindowW(kWndClass, nullptr);
            if (other) PostMessageW(other, WM_APP_RUNNING, 0, 0);
            CloseHandle(mutex);
            return 0;
        }
        // A diagnostic run must be possible while the daily-driver instance keeps the tray:
        // selftest, a replay or a dialog probe is a separate short-lived process.
        Log(L"INFO", L"debug mode: running alongside the live instance");
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    DetectCores();

    g.deviceId    = RegGetStr(L"DeviceId");
    g.modelFolder = RegGetStr(L"ModelFolder");
    g.hkMods      = RegGetDword(L"HotkeyMods", MOD_CONTROL | MOD_ALT);
    g.hkVk        = RegGetDword(L"HotkeyVk", VK_SPACE);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    // A message-only window (HWND_MESSAGE) cannot host a tray icon: the shell needs a real
    // top-level window to post to. Create a normal window and simply never show it.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, kAppName, WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g.hwnd) return 1;

    // Offline polish harness (verification only, no tray, no dictation):
    //   --polish-file  <in.txt> <out.txt>   punctuate a UTF-8 text file
    //   --polish-tokens <in.txt> <out.txt>  dump "id start end piece" per subtoken
    if (debug) {
        int nac = 0;
        LPWSTR* nav = CommandLineToArgvW(GetCommandLineW(), &nac);
        for (int i = 1; nav && i + 2 < nac; ++i) {
            bool toks = !_wcsicmp(nav[i], L"--polish-tokens");
            if (toks || !_wcsicmp(nav[i], L"--polish-file")) {
                HANDLE h = CreateFileW(nav[i + 1], GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                std::string raw;
                if (h != INVALID_HANDLE_VALUE) {
                    char buf[65536]; DWORD got;
                    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
                    CloseHandle(h);
                }
                if (!raw.empty() && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
                std::wstring text;
                if (!raw.empty()) {
                    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
                    text.resize(n);
                    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &text[0], n);
                }
                if (!punct::Init(ExeDir() + L"\\models\\punctuation", PolishLog, 1)) {
                    fwprintf(stderr, L"polish model init failed\n");
                    return 2;
                }
                bool ok = toks && punct::DumpTokens(text, nav[i + 2]);
                if (!toks) {
                    std::wstring out = punct::Polish(text);
                    std::string u8(out.size() * 3 + 4, '\0');
                    int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), (int)out.size(),
                                                u8.data(), (int)u8.size(), nullptr, nullptr);
                    h = CreateFileW(nav[i + 2], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        DWORD wrote;
                        WriteFile(h, u8.data(), (DWORD)n, &wrote, nullptr);
                        CloseHandle(h);
                        ok = true;
                    }
                }
                if (nav) LocalFree(nav);
                return ok ? 0 : 3;
            }
        }
        if (nav) LocalFree(nav);
    }

    InitNotify();
    InitEyeball();
    InstallForegroundHook();
    StartInjector();
    // Polish ships OFF. The Nemotron checkpoint punctuates natively and
    // well enough; the rewrite pass's UX needs more work before it may surprise a
    // fresh install. Users opt IN from the tray (restart-scoped, as always).
    g_polishEnabled = RegGetDword(L"Polish", 0) != 0;
    if (g_polishEnabled)
        CreateThread(nullptr, 0, PolishInitThread, nullptr, 0, nullptr);
    Log(L"INFO", std::wstring(L"startup: Tiro ") + kAppVersion + L" (" TIRO_GIT_HASH L")");
    if (debug) Log(L"INFO", L"debug mode: diagnostic switches enabled, single instance not enforced");
    SetState(kLoading);
    SetTimer(g.hwnd, IDT_LOAD, 1200, nullptr);

    if (g.hkVk) {
        if (!RegisterHotKey(g.hwnd, HOTKEY_TOGGLE, g.hkMods | MOD_NOREPEAT, g.hkVk))
            Log(L"WARN", L"hotkey registration failed at startup");
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (debug && !_wcsicmp(argv[i], L"--rewrite-demo")) {
            // End-to-end check of the one-shot rewrite against a RICHEDIT50W control
            // that Tiro owns (Win11 Notepad is WinUI and cannot be driven or read this
            // way; a classic edit window can).
            Log(L"INFO", L"rewrite-demo: begin");
            const wchar_t* raw = L"hello world this is raw dictated text without any punctuation at all and it keeps going because short text would hide the selection crawl that a user actually sees when they dictate a real paragraph into an application on their machine today";
            const wchar_t* pol = L"Hello world. This is raw dictated text without any punctuation at all, and it keeps going because short text would hide the selection crawl that a user actually sees when they dictate a real paragraph into an application on their machine today.";
            // The real tray window's IDT_LOAD timer would otherwise fire inside our pumps and
            // boot the ASR engine under the demo; silence it.
            KillTimer(g.hwnd, IDT_LOAD);
            WNDCLASSEXW wcd = { sizeof(wcd) };
            wcd.lpfnWndProc = DefWindowProcW;
            wcd.hInstance = GetModuleHandleW(nullptr);
            wcd.lpszClassName = L"TiroRewriteDemo";
            RegisterClassExW(&wcd);
            HWND top = CreateWindowExW(WS_EX_TOPMOST, wcd.lpszClassName, L"Tiro rewrite demo",
                                       WS_OVERLAPPEDWINDOW, 200, 200, 600, 300,
                                       nullptr, nullptr, wcd.hInstance, nullptr);
            // Msftedit.dll lives in System32; a bare name would search this app's own
            // folder first, and Tiro is portable - it normally runs from Downloads,
            // the Desktop, or wherever it was unzipped. Never load a DLL from there.
            LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);   // RICHEDIT50W lives here (riched20 is the older class)
            HWND ed = CreateWindowExW(0, L"RICHEDIT50W", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
                                      10, 10, 560, 260, top, nullptr, wc.hInstance, nullptr);
            wchar_t edg[120];
            swprintf_s(edg, L"rewrite-demo: PROBE edit hwnd=%p err=%u", (void*)ed, GetLastError());
            Log(L"INFO", edg);
            ShowWindow(top, SW_SHOW);
            Log(L"INFO", L"rewrite-demo: window shown");
            // Take the foreground without AttachThreadInput (that call blocks on a busy
            // foreground thread). ALT nudge + SwitchToThisWindow is the allowed path from a
            // background process; retry until our window actually owns the foreground.
            for (int t = 0; t < 20 && GetForegroundWindow() != top; ++t) {
                keybd_event(VK_MENU, 0, 0, 0); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
                SwitchToThisWindow(top, TRUE);
                SetForegroundWindow(top);
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                Sleep(100);
            }
            SetFocus(ed);
            Log(L"INFO", L"rewrite-demo: focused, pumping");
            // This thread owns the EDIT control: it must pump, or injected keys never run
            // and WM_GETTEXT would deadlock. Pump for roughly d milliseconds.
            auto Pump = [](int d) {
                ULONGLONG e = GetTickCount64() + d;
                do {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                    Sleep(10);
                } while ((long long)(e - GetTickCount64()) > 0);
            };
            Pump(400);
            Log(L"INFO", L"rewrite-demo: first pump done");
            wchar_t fg[128] = L"?";
            HWND f = GetForegroundWindow();
            if (f) GetClassNameW(f, fg, 128);
            SendUnicode(raw);
            Log(L"INFO", L"rewrite-demo: keys sent");
            Pump(400);
            Log(L"INFO", L"rewrite-demo: typed, spawning rewrite thread");
            wchar_t p0[256] = L"(empty)";
            SendMessageW(ed, WM_GETTEXT, 255, (LPARAM)p0);
            wchar_t e0[300];
            swprintf_s(e0, L"rewrite-demo: PROBE pre-rewrite-edit=\"%ls\"", p0);
            Log(L"INFO", e0);
            // Run the rewrite on a worker thread exactly as PolishThread does in production,
            // while this (the target's) thread pumps.
            struct RWJob { HWND top; std::wstring raw; const wchar_t* pol; bool ok; }
                job = { top, raw, pol, false };
            ULONGLONG t0 = GetTickCount64();
            HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
                RWJob* j = (RWJob*)p;
                j->ok = RewriteViaReplaceSel(j->top, j->raw, j->pol);
                Log(j->ok ? L"INFO" : L"ERROR",
                    j->ok ? L"rewrite-demo: replaced via EM_REPLACESEL"
                          : L"rewrite-demo: EM_REPLACESEL failed on own control");
                return 0; }, &job, 0, nullptr);
            Pump(1500);
            if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
            ULONGLONG ms = GetTickCount64() - t0;
            bool ok = job.ok;
            Pump(500);
            wchar_t early[256] = L"(empty)";
            SendMessageW(ed, WM_GETTEXT, 255, (LPARAM)early);
            wchar_t eb[300];
            swprintf_s(eb, L"rewrite-demo: PROBE early-edit=\"%ls\"", early);
            Log(L"INFO", eb);
            wchar_t got[256] = L"(empty)";
            SendMessageW(ed, WM_GETTEXT, 255, (LPARAM)got);
            bool textOk = wcscmp(got, pol) == 0;
            wchar_t b[512];
            swprintf_s(b, L"rewrite-demo: %ls | edit=\"%ls\" | fg=%ls | rewrite %s in %llu ms",
                       (textOk && ok) ? L"PASS" : L"FAIL", got, fg,
                       ok ? L"ok" : L"failed", ms);
            Log(textOk && ok ? L"INFO" : L"ERROR", b);
            DestroyWindow(top);
            if (argv) LocalFree(argv);
            CoUninitialize();
            return 0;
        }
        if (debug && !_wcsicmp(argv[i], L"--probe-focus")) {
            // Read-only interrogation of the control that currently has keyboard focus,
            // so a test harness can record which rewrite path each target supports.
            // Waits 2.5 s first: the harness focuses the target, then runs this.
            // Modifies nothing - only GET/query messages are sent.
            Sleep(2500);
            HWND top = GetForegroundWindow();
            if (!top) { Log(L"ERROR", L"probe: no foreground window"); return 0; }
            wchar_t topCls[80] = L"?", topTtl[160] = L"", proc[160] = L"?";
            GetClassNameW(top, topCls, 80);
            GetWindowTextW(top, topTtl, 160);
            DWORD pid = 0; GetWindowThreadProcessId(top, &pid);
            { HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
              if (hp) { DWORD n = 160; QueryFullProcessImageNameW(hp, 0, proc, &n); CloseHandle(hp); } }
            HWND ed = FocusedEditOf(top);
            wchar_t edCls[80] = L"(none)";
            if (ed) GetClassNameW(ed, edCls, 80);
            wchar_t b[512];
            swprintf_s(b, L"probe: fg=\"%ls\" [%ls] proc=%ls pid=%lu focused=%ls",
                       topTtl, topCls, wcsrchr(proc, L'\\') ? wcsrchr(proc, L'\\') + 1 : proc, pid, edCls);
            Log(L"INFO", b);
            // Classic edit messages (read-only).
            BOOL richLike = FALSE, editLike = FALSE;
            for (wchar_t* p = edCls; *p; ++p) *p = towlower(*p);
            if (ed) {
                if (wcsstr(edCls, L"richedit")) richLike = TRUE;
                else if (wcscmp(edCls, L"edit") == 0) editLike = TRUE;
            }
            if (ed && (richLike || editLike)) {
                DWORD_PTR res = 0;
                BOOL alive = SendMessageTimeoutW(ed, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 200, &res) != 0;
                DWORD sel = 0; BOOL r = SendMessageTimeoutW(ed, EM_GETSEL, 0, (LPARAM)&sel, SMTO_ABORTIFHUNG, 200, nullptr) != 0;
                swprintf_s(b, L"probe: edit msgs alive=%d textlen=%lu caret=%u", alive, (unsigned long)res, r ? LOWORD(sel) : 0);
                Log(L"INFO", b);
            }
            // UIA patterns on the focused element.
            BOOL hasText = FALSE, hasValue = FALSE, valueRO = TRUE;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IUIAutomation* uia = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&uia)))) {
                IUIAutomationElement* el = nullptr;
                if (SUCCEEDED(uia->GetFocusedElement(&el)) && el) {
                    IUnknown* p = nullptr;
                    if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, &p))) { hasText = TRUE; p->Release(); }
                    p = nullptr;
                    if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, &p))) {
                        hasValue = TRUE;
                        IUIAutomationValuePattern* vp = nullptr;
                        if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&vp)))) { vp->get_CurrentIsReadOnly(&valueRO); vp->Release(); }
                        p->Release();
                    }
                    el->Release();
                }
                uia->Release();
            }
            swprintf_s(b, L"probe: uia TextPattern=%d ValuePattern=%d%s", hasText, hasValue,
                       hasValue ? (valueRO ? L" (readonly)" : L" (writable)") : L"");
            Log(L"INFO", b);
            // Verdict: whether the one-shot rewrite would apply to this target.
            const wchar_t* verdict =
                (richLike || editLike) ? L"one-shot EM_REPLACESEL applies"
              : hasValue && !valueRO   ? L"one-shot UIA SetValue applies"
                                       : L"unsupported - text will be left as typed";
            swprintf_s(b, L"probe: verdict=%ls", verdict);
            Log(L"INFO", b);
            wprintf(L"%s\n", b);
            CoUninitialize();
            return 0;
        }
        if (debug && !_wcsicmp(argv[i], L"--live") && i + 1 < argc) {
            // Feeds a WAV through the same queue the microphone fills, so endpointing and
            // injection are exercised exactly as they are live.
            g.modelFolder = RegGetStr(L"ModelFolder");
            if (!ReadWav16kMono(argv[i + 1], g_feeder)) {
                Log(L"ERROR", L"live test: expected 16 kHz mono 16-bit WAV");
                if (argv) LocalFree(argv);
                CoUninitialize();
                return 0;
            }
            g_feeder.resize(g_feeder.size() + 32000, 0.0f);   // 2 s of trailing silence:
                                                               // a real microphone always supplies it,
                                                               // and it is what triggers the endpoint
            DetectCores();
            StartEngine();
            g.state  = kOn;                 // exercise the real injection path
            SetTimer(g.hwnd, IDT_METER, 100, nullptr);   // and the flush that rides on it
            g.target = PickTarget();
            // A test harness can name the window instead of racing for the foreground: Windows
            // blocks SetForegroundWindow from a background process, so "whatever is in front"
            // is not something a script can promise. Delivery still requires the window to
            // actually hold focus, which is the behaviour under test.
            for (int j = 1; j + 1 < argc; ++j) {
                if (!_wcsicmp(argv[j], L"--target")) {
                    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, nullptr, nullptr)) != nullptr; ) {
                        wchar_t t[128]{};
                        GetWindowTextW(h, t, 128);
                        if (wcsstr(t, argv[j + 1]) && IsWindowVisible(h)) { g.target = h; break; }
                    }
                    break;
                }
            }
            {
                wchar_t cls[64]{}; wchar_t title[128]{};
                GetClassNameW(g.target, cls, 64);
                GetWindowTextW(g.target, title, 128);
                Log(L"INFO", std::wstring(L"live test: target pinned (") + title + L", " +
                    cls + L"), feeding audio");
            }
            CreateThread(nullptr, 0, FeederThread, nullptr, 0, nullptr);
            if (argv) LocalFree(argv);
            argv = nullptr;
            argc = 0;
            break;              // fall through to the normal message loop
        }
        if (debug && !_wcsicmp(argv[i], L"--selftest") && i + 1 < argc) {
            g.modelFolder = RegGetStr(L"ModelFolder");
            RunSelfTest(argv[i + 1]);
            if (argv) LocalFree(argv);
            CoUninitialize();
            return 0;
        }
    }
    // Diagnostic switches: --debug --about | --hotkey | --notify | --startup-toggle | --restart.
    // Without --debug they are inert, and each one says so once in the log.
    for (int i = 1; argv && i < argc; ++i) {
        const bool known = !_wcsicmp(argv[i], L"--about") || !_wcsicmp(argv[i], L"--hotkey") ||
                           !_wcsicmp(argv[i], L"--notify") || !_wcsicmp(argv[i], L"--startup-toggle") ||
                           !_wcsicmp(argv[i], L"--restart") || !_wcsicmp(argv[i], L"--selftest") ||
                           !_wcsicmp(argv[i], L"--live") || !_wcsicmp(argv[i], L"--rewrite-demo") ||
                           !_wcsicmp(argv[i], L"--probe-focus") ||
                           !_wcsicmp(argv[i], L"--polish-file") || !_wcsicmp(argv[i], L"--polish-tokens");
        if (!debug) {
            if (known)
                Log(L"INFO", std::wstring(L"diagnostic switch ignored (needs --debug): ") + argv[i]);
            continue;
        }
        if (!_wcsicmp(argv[i], L"--about"))  PostMessageW(g.hwnd, WM_APP_TEST_ABOUT, 0, 0);
        if (!_wcsicmp(argv[i], L"--startup-toggle"))  PostMessageW(g.hwnd, WM_APP_TEST_STARTUP, 0, 0);
        if (!_wcsicmp(argv[i], L"--restart"))  PostMessageW(g.hwnd, WM_APP_TEST_RESTART, 0, 0);
        if (!_wcsicmp(argv[i], L"--hotkey")) PostMessageW(g.hwnd, WM_APP_TEST_HOTKEY, 0, 0);
        if (!_wcsicmp(argv[i], L"--notify")) PostMessageW(g.hwnd, WM_APP_TEST_NOTIFY, 0, 0);
    }
    if (argv) LocalFree(argv);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    {
        std::lock_guard<std::mutex> lk(g_injectMu);
        g_injectStop = true;
    }
    g_injectCv.notify_all();
    if (g_injectThread) {
        WaitForSingleObject(g_injectThread, 5000);
        CloseHandle(g_injectThread);
        g_injectThread = nullptr;
    }
    UnloadEngine();
    CloseCapture();
    if (g.hkVk) UnregisterHotKey(g.hwnd, HOTKEY_TOGGLE);
    Log(L"INFO", L"shutdown");
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
