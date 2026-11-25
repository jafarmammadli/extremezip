// ExtremeExtract.exe — Win32 front end. The engine runs on its own thread; the UI only
// polls the engine's atomic counters on a 100 ms timer, so it can never slow extraction.
#include "engine.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

enum { ID_ARCHIVE = 100, ID_OPEN, ID_DEST, ID_BROWSE, ID_EXTRACT, ID_PAUSE, ID_CANCEL, ID_PROGRESS, ID_STATS, ID_ERRORS, ID_DROP, ID_TIMER };

using clock_t_ = std::chrono::steady_clock;

struct App {
    HWND wnd{}, drop{}, archive{}, open{}, dest{}, browse{}, extract{}, pause{}, cancel{}, progress{}, stats{}, errors{};
    HFONT font{}, big_font{};
    bool dark = false;
    HBRUSH bg{}, field{};
    COLORREF text = 0;

    std::unique_ptr<xx::Progress> prog;
    std::jthread worker;
    std::string fatal;           // written by the worker before done is set
    std::atomic<bool> done{true}; // worker finished (set after fatal, unlike Progress::finished)
    clock_t_::time_point t0, last_t;
    uint64_t last_bytes = 0, last_cpu = 0;
    double speed = 0;  // smoothed MiB/s
    size_t errors_shown = 0;
} app;

std::wstring get_text(HWND h) {
    std::wstring s(GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), (int)s.size() + 1);
    return s;
}

std::wstring default_dest(const std::wstring& archive) {
    std::wstring d = archive;
    if (auto dot = d.find_last_of(L'.'); dot != std::wstring::npos && dot > d.find_last_of(L"\\/") + 1) d.resize(dot);
    return d;
}

void set_archive(const std::wstring& path) {
    SetWindowTextW(app.archive, path.c_str());
    SetWindowTextW(app.dest, default_dest(path).c_str());
    SetWindowTextW(app.drop, (L"⬇  " + path.substr(path.find_last_of(L"\\/") + 1)).c_str());
}

std::wstring pick(bool folder) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD o;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    COMDLG_FILTERSPEC zip[] = {{L"ZIP archives", L"*.zip"}, {L"All files", L"*.*"}};
    if (!folder) dlg->SetFileTypes(2, zip);
    IShellItem* item;
    if (SUCCEEDED(dlg->Show(app.wnd)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR p;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
        item->Release();
    }
    dlg->Release();
    return result;
}

uint64_t process_cpu_100ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

bool running() { return !app.done.load(); }

void update_buttons() {
    bool run = running();
    EnableWindow(app.extract, !run);
    EnableWindow(app.pause, run);
    EnableWindow(app.cancel, run);
    SetWindowTextW(app.pause, run && app.prog->pause ? L"Resume" : L"Pause");
}

std::wstring fmt_time(double s) {
    wchar_t b[32];
    if (s < 60) swprintf(b, 32, L"%.0f s", s);
    else swprintf(b, 32, L"%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

void tick() {
    if (!app.prog) return;
    xx::Progress& p = *app.prog;
    auto now = clock_t_::now();
    double dt = std::chrono::duration<double>(now - app.last_t).count();
    double el = std::chrono::duration<double>(now - app.t0).count();
    uint64_t bytes = p.bytes_done.load(), total = p.bytes_total.load(), cpu = process_cpu_100ns();
    double inst = (bytes - app.last_bytes) / 1048576.0 / std::max(dt, 1e-3);
    app.speed = app.speed == 0 ? inst : app.speed * 0.7 + inst * 0.3;
    double cpu_pct = (cpu - app.last_cpu) / 1e7 / std::max(dt, 1e-3) / std::thread::hardware_concurrency() * 100;
    app.last_t = now, app.last_bytes = bytes, app.last_cpu = cpu;
    double avg = bytes / 1048576.0 / std::max(el, 1e-3);

    PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    SendMessageW(app.progress, PBM_SETPOS, total ? (WPARAM)(bytes * 1000 / total) : 0, 0);

    bool done = app.done.load();
    wchar_t s[512];
    if (done) {
        swprintf(s, 512, L"%ls  %llu files, %.1f MiB in %.2f s  •  average %.0f MiB/s  •  %u threads\n%zu errors",
                 p.cancel ? L"Cancelled." : L"Done.", p.files_done.load(), bytes / 1048576.0, el, avg, p.threads.load(), p.error_count.load());
    } else {
        double eta = avg > 0 ? (total - bytes) / 1048576.0 / avg : 0;
        swprintf(s, 512, L"Speed %.0f MiB/s  (average %.0f)  •  ETA %ls%ls\nFiles %llu / %llu  •  %.1f / %.1f MiB\nCPU %.0f%%  •  RAM %llu MB  •  %u threads",
                 p.pause ? 0.0 : app.speed, avg, fmt_time(eta).c_str(), p.pause ? L"  (paused)" : L"",
                 p.files_done.load(), p.files_total.load(), bytes / 1048576.0, total / 1048576.0, cpu_pct,
                 (unsigned long long)(pmc.WorkingSetSize >> 20), p.threads.load());
    }
    if (!app.fatal.empty()) swprintf(s, 512, L"Failed: %hs", app.fatal.c_str());
    SetWindowTextW(app.stats, s);

    if (p.error_count.load() != app.errors_shown) {
        auto errs = p.errors();
        for (; app.errors_shown < errs.size(); ++app.errors_shown) SendMessageW(app.errors, LB_ADDSTRING, 0, (LPARAM)errs[app.errors_shown].c_str());
    }
    if (done) {
        KillTimer(app.wnd, ID_TIMER);
        app.worker = {};
        update_buttons();
    }
}

void start() {
    std::wstring archive = get_text(app.archive), dest = get_text(app.dest);
    if (archive.empty()) { MessageBoxW(app.wnd, L"Choose or drop a ZIP archive first.", L"Extreme Extract", MB_ICONINFORMATION); return; }
    if (dest.empty()) dest = default_dest(archive);
    SendMessageW(app.errors, LB_RESETCONTENT, 0, 0);
    app.errors_shown = 0;
    app.fatal.clear();
    app.done = false;
    app.prog = std::make_unique<xx::Progress>();
    app.t0 = app.last_t = clock_t_::now();
    app.last_bytes = 0, app.speed = 0, app.last_cpu = process_cpu_100ns();
    app.worker = std::jthread([archive, dest, p = app.prog.get()] {
        try {
            xx::extract(archive, dest, xx::Options{}, *p);
        } catch (const std::exception& e) {
            app.fatal = e.what();
        }
        app.done = true;
    });
    SetTimer(app.wnd, ID_TIMER, 100, nullptr);
    update_buttons();
}

void resume_or_cancel(bool cancel) {
    xx::Progress& p = *app.prog;
    if (cancel) p.cancel = true;
    p.pause = cancel ? false : !p.pause.load();
    p.pause.notify_all();
    update_buttons();
}

void layout() {
    RECT rc;
    GetClientRect(app.wnd, &rc);
    const double s = GetDpiForWindow(app.wnd) / 96.0;
    auto px = [&](int v) { return (int)(v * s); };
    const int m = px(16), w = rc.right - 2 * m, bw = px(96), h = px(28), gap = px(8);
    auto put = [](HWND c, int x, int y, int cw, int ch) { SetWindowPos(c, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE); };
    int y = m;
    put(app.drop, m, y, w, px(72)); y += px(72) + gap * 2;
    put(app.archive, m, y, w - bw - gap, h); put(app.open, m + w - bw, y, bw, h); y += h + gap;
    put(app.dest, m, y, w - bw - gap, h); put(app.browse, m + w - bw, y, bw, h); y += h + gap * 2;
    put(app.extract, m, y, bw, h); put(app.pause, m + bw + gap, y, bw, h); put(app.cancel, m + 2 * (bw + gap), y, bw, h); y += h + gap * 2;
    put(app.progress, m, y, w, px(14)); y += px(14) + gap * 2;
    put(app.stats, m, y, w, px(60)); y += px(60) + gap;
    put(app.errors, m, y, w, std::max<int>(rc.bottom - m - y, px(40)));
}

void apply_fonts() {
    if (app.font) DeleteObject(app.font), DeleteObject(app.big_font);
    const UINT dpi = GetDpiForWindow(app.wnd);
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    app.font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 3 / 2;
    app.big_font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND c : {app.archive, app.open, app.dest, app.browse, app.extract, app.pause, app.cancel, app.stats, app.errors})
        SendMessageW(c, WM_SETFONT, (WPARAM)app.font, TRUE);
    SendMessageW(app.drop, WM_SETFONT, (WPARAM)app.big_font, TRUE);
}

void create_controls(HWND w) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, w, (HMENU)(INT_PTR)id, hi, nullptr);
    };
    app.drop = mk(L"STATIC", L"Drop a ZIP archive here", SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY | WS_BORDER, ID_DROP);
    app.archive = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_ARCHIVE, WS_EX_CLIENTEDGE);
    app.open = mk(L"BUTTON", L"Open…", BS_PUSHBUTTON | WS_TABSTOP, ID_OPEN);
    app.dest = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_DEST, WS_EX_CLIENTEDGE);
    app.browse = mk(L"BUTTON", L"Destination…", BS_PUSHBUTTON | WS_TABSTOP, ID_BROWSE);
    app.extract = mk(L"BUTTON", L"Extract", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_EXTRACT);
    app.pause = mk(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, ID_PAUSE);
    app.cancel = mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL);
    app.progress = mk(PROGRESS_CLASSW, L"", 0, ID_PROGRESS);
    app.stats = mk(L"STATIC", L"Ready.", SS_LEFT, ID_STATS);
    app.errors = mk(L"LISTBOX", L"", LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL, ID_ERRORS, WS_EX_CLIENTEDGE);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(app.archive, EM_SETCUEBANNER, TRUE, (LPARAM)L"Archive");
    SendMessageW(app.dest, EM_SETCUEBANNER, TRUE, (LPARAM)L"Destination folder");
    SendMessageW(app.errors, LB_SETHORIZONTALEXTENT, 4000, 0);
    if (app.dark)
        for (HWND c : {app.open, app.browse, app.extract, app.pause, app.cancel, app.errors}) SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
    if (app.dark)
        for (HWND c : {app.archive, app.dest}) SetWindowTheme(c, L"DarkMode_CFD", nullptr);
    apply_fonts();
    layout();
    update_buttons();
}

LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: app.wnd = w; create_controls(w); DragAcceptFiles(w, TRUE); return 0;
        case WM_SIZE: layout(); return 0;
        case WM_DPICHANGED: {
            auto r = (RECT*)lp;
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            apply_fonts();
            layout();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            double s = GetDpiForWindow(w) / 96.0;
            ((MINMAXINFO*)lp)->ptMinTrackSize = {(LONG)(480 * s), (LONG)(440 * s)};
            return 0;
        }
        case WM_DROPFILES: {
            wchar_t path[32768];
            if (DragQueryFileW((HDROP)wp, 0, path, 32768)) set_archive(path);
            DragFinish((HDROP)wp);
            return 0;
        }
        case WM_TIMER: tick(); return 0;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_DROP: if (HIWORD(wp) != STN_CLICKED) break; [[fallthrough]];
                case ID_OPEN: if (auto p = pick(false); !p.empty()) set_archive(p); break;
                case ID_BROWSE: if (auto p = pick(true); !p.empty()) SetWindowTextW(app.dest, p.c_str()); break;
                case ID_EXTRACT: start(); break;
                case ID_PAUSE: if (running()) resume_or_cancel(false); break;
                case ID_CANCEL: if (running()) resume_or_cancel(true); break;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (!app.dark) break;
            SetTextColor((HDC)wp, app.text);
            SetBkColor((HDC)wp, msg == WM_CTLCOLORSTATIC ? RGB(32, 32, 32) : RGB(45, 45, 45));
            return (LRESULT)(msg == WM_CTLCOLORSTATIC ? app.bg : app.field);
        case WM_CLOSE:
            if (running()) resume_or_cancel(true);
            app.worker = {};  // join: the engine deletes partial files on cancel
            DestroyWindow(w);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

bool system_dark() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    app.dark = system_dark();
    app.bg = CreateSolidBrush(app.dark ? RGB(32, 32, 32) : GetSysColor(COLOR_BTNFACE));
    app.field = CreateSolidBrush(RGB(45, 45, 45));
    app.text = RGB(235, 235, 235);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = app.bg;
    wc.lpszClassName = L"ExtremeExtract";
    RegisterClassExW(&wc);

    const double s = GetDpiForSystem() / 96.0;
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"Extreme Extract", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             (int)(640 * s), (int)(560 * s), nullptr, nullptr, hi, nullptr);
    BOOL dark = app.dark;
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);

    int argc;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc); argc > 1) set_archive(argv[1]);

    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
