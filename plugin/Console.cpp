// KenshiKin: the session's log in one place, and the host's window outside the game.
//
//  Client : its own log lines (everything log() writes) go to the host twice a second
//           (MSG_LOG, share_logs=1), so a session can be debugged from the host's files alone.
//  Host   : the clients' lines are written into its own log as "[name] ..." and, with every line
//           of its own, into the host console: a plain Windows window (its own thread, never
//           touching the game) with the players, the live log and a command line. It opens
//           minimised (no focus stolen from the game) when hosting starts (host_console=1), or
//           with /console. Commands go to the main loop (logs_popCommand), which runs them.
// Idea from LRaverdy/KenshiCoop (client journals on the host, host console); written anew here.
#include <windows.h>
#include <Debug.h>

#include "Shared.h"

#include <deque>
#include <string>
#include <vector>

using namespace mp;

namespace kmp {

namespace
{
    CRITICAL_SECTION g_cs;                 // leaf lock: log() may run on any thread
    bool g_csReady = false;
    struct CsLock { CsLock() { EnterCriticalSection(&g_cs); } ~CsLock() { LeaveCriticalSection(&g_cs); } };

    // Lines for the console (host) and for the host (client).
    std::deque<std::string> g_ring;        // last lines, for the console window
    long g_ringFirst = 0;                  // sequence number of g_ring.front()
    const size_t RING_MAX = 4000;
    std::vector<std::string> g_outgoing;   // client: not sent yet
    long g_outDropped = 0;
    const size_t OUT_MAX = 400, LINE_MAX = 300, PER_MSG = 80;
    DWORD g_lastFlush = 0;

    std::deque<std::string> g_commands;    // typed in the console, run by the main loop
    std::string g_playersText;             // shown at the top of the console (set by the main loop)

    void ringPush(const std::string& line)
    {
        g_ring.push_back(line);
        while (g_ring.size() > RING_MAX) { g_ring.pop_front(); ++g_ringFirst; }
    }

    std::string timestamp()
    {
        SYSTEMTIME t; GetLocalTime(&t);
        char b[16]; sprintf_s(b, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        return b;
    }

    // ---- the window (its own thread) -------------------------------------------------------
    HWND g_wnd = NULL, g_logEdit = NULL, g_input = NULL, g_players = NULL, g_resyncBtn = NULL;
    HANDLE g_thread = NULL;
    volatile LONG g_showRequest = 0;       // 1: show (restore), set from the main thread
    long g_shownSeq = 0;                   // next ring line to append (window thread)
    std::string g_shownPlayers;
    WNDPROC g_inputProc = NULL;
    HFONT g_font = NULL;
    const UINT_PTR TIMER_ID = 1;
    enum { ID_RESYNC = 101 };

    std::wstring widen(const std::string& s)
    {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), NULL, 0);
        UINT cp = CP_UTF8;
        if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), NULL, 0); }
        if (n <= 0) return std::wstring();
        std::wstring w(n, L'\0');
        MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    std::string narrowUtf8(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s(n > 0 ? n : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
        return s;
    }

    void layout()
    {
        RECT r; GetClientRect(g_wnd, &r);
        int w = r.right, h = r.bottom, top = 72, bottom = 28, bw = 140;
        MoveWindow(g_players, 6, 6, w - bw - 18, top - 10, TRUE);
        MoveWindow(g_resyncBtn, w - bw - 6, 6, bw, 28, TRUE);
        MoveWindow(g_logEdit, 6, top, w - 12, h - top - bottom - 10, TRUE);
        MoveWindow(g_input, 6, h - bottom - 4, w - 12, bottom, TRUE);
    }

    void appendNewLines()
    {
        std::wstring text;
        std::string players;
        {
            CsLock l;
            if (g_shownSeq < g_ringFirst) g_shownSeq = g_ringFirst;
            long end = g_ringFirst + (long)g_ring.size();
            std::string chunk;
            for (long i = g_shownSeq; i < end; ++i) { chunk += g_ring[(size_t)(i - g_ringFirst)]; chunk += "\r\n"; }
            g_shownSeq = end;
            text = widen(chunk);
            players = g_playersText;
        }
        if (players != g_shownPlayers) { g_shownPlayers = players; SetWindowTextW(g_players, widen(players).c_str()); }
        if (text.empty()) return;
        int len = GetWindowTextLengthW(g_logEdit);
        if (len > 400000)   // keep the edit control light: drop the older half
        {
            SendMessageW(g_logEdit, EM_SETSEL, 0, len / 2);
            SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, (LPARAM)L"[...]\r\n");
            len = GetWindowTextLengthW(g_logEdit);
        }
        SendMessageW(g_logEdit, EM_SETSEL, len, len);
        SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    }

    LRESULT CALLBACK inputProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_KEYDOWN && w == VK_RETURN)
        {
            int n = GetWindowTextLengthW(h);
            std::wstring s(n + 1, L'\0');
            GetWindowTextW(h, &s[0], n + 1);
            s.resize(n);
            SetWindowTextW(h, L"");
            std::string cmd = narrowUtf8(s);
            if (!cmd.empty()) { CsLock lk; if (g_commands.size() < 64) g_commands.push_back(cmd); ringPush(timestamp() + "> " + cmd); }
            return 0;
        }
        if (m == WM_CHAR && w == VK_RETURN) return 0;   // no beep
        return CallWindowProcW(g_inputProc, h, m, w, l);
    }

    LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        switch (m)
        {
        case WM_SIZE: if (g_logEdit) layout(); return 0;
        case WM_TIMER:
            if (InterlockedExchange(&g_showRequest, 0)) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
            appendNewLines();
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == ID_RESYNC) { CsLock lk; g_commands.push_back("resync all"); ringPush(timestamp() + "> resync all"); }
            return 0;
        case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;   // /console shows it again
        }
        return DefWindowProcW(h, m, w, l);
    }

    DWORD WINAPI windowThread(LPVOID)
    {
        HINSTANCE inst = GetModuleHandleW(NULL);
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"KenshiKinHostConsole";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassExW(&wc);
        g_wnd = CreateWindowExW(0, wc.lpszClassName, L"KenshiKin - host console", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, NULL, NULL, inst, NULL);
        if (!g_wnd) return 0;
        g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        g_players = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_wnd, NULL, inst, NULL);
        g_resyncBtn = CreateWindowExW(0, L"BUTTON", L"Resync everyone", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, g_wnd, (HMENU)ID_RESYNC, inst, NULL);
        g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 10, 10, g_wnd, NULL, inst, NULL);
        g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_wnd, NULL, inst, NULL);
        SendMessageW(g_logEdit, EM_SETLIMITTEXT, 0, 0);
        HWND all[4] = { g_players, g_resyncBtn, g_logEdit, g_input };
        for (int i = 0; i < 4; ++i) SendMessageW(all[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        g_inputProc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)inputProc);
        layout();
        ShowWindow(g_wnd, SW_SHOWMINNOACTIVE);   // in the taskbar, the game keeps the focus
        SetTimer(g_wnd, TIMER_ID, 250, NULL);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
}

void logs_init()
{
    if (g_csReady) return;
    InitializeCriticalSection(&g_cs);
    g_csReady = true;
}

// Every line log() writes (any thread).
void logs_capture(const char* line)
{
    if (!g_csReady || !line) return;
    std::string s(line);
    if (s.size() > LINE_MAX) s.resize(LINE_MAX);
    bool share = g_cfg.shareLogs && g_session.active() && !g_session.isHost();   // before our lock (lock order)
    std::string stamped = timestamp() + s;
    CsLock l;
    ringPush(stamped);
    if (share)
    {
        if (g_outgoing.size() < OUT_MAX) g_outgoing.push_back(s);
        else ++g_outDropped;
    }
}

// Host: a client's lines.
void logs_onMessage(const NetEvent& e)
{
    if (!g_session.isHost() || e.sender == HOST_ID) return;
    ByteReader r(e.body);
    if (r.u8() != HOST_ID) return;
    uint16_t n = r.u16();
    if (!r.ok() || n > PER_MSG + 1) return;
    std::string who = playerName(e.sender);
    for (uint16_t i = 0; i < n && r.ok(); ++i)
    {
        std::string line = r.str();
        if (!r.ok()) break;
        if (line.size() > LINE_MAX) line.resize(LINE_MAX);
        for (size_t k = 0; k < line.size(); ++k) if (line[k] == '\r' || line[k] == '\n') line[k] = ' ';
        std::string full = "[" + who + "] " + line;
        DebugLog("KenshiKin: " + full);   // not log(): it is not ours to forward again
        CsLock l;
        ringPush(timestamp() + full);
    }
}

void logs_tick(DWORD now)
{
    if (!g_csReady) return;
    if (!g_session.isHost() && g_session.active() && g_cfg.shareLogs && now - g_lastFlush >= 500)
    {
        g_lastFlush = now;
        std::vector<std::string> lines;
        long dropped = 0;
        {
            CsLock l;
            size_t n = g_outgoing.size() < PER_MSG ? g_outgoing.size() : PER_MSG;
            lines.assign(g_outgoing.begin(), g_outgoing.begin() + n);
            g_outgoing.erase(g_outgoing.begin(), g_outgoing.begin() + n);
            dropped = g_outDropped; g_outDropped = 0;
        }
        if (dropped) { char b[64]; sprintf_s(b, "(%ld log line(s) not sent: too many)", dropped); lines.push_back(b); }
        if (!lines.empty())
        {
            ByteWriter w; w.u8(HOST_ID); w.u16((uint16_t)lines.size());
            for (size_t i = 0; i < lines.size(); ++i) w.str(lines[i]);
            g_session.sendTo(HOST_ID, MSG_LOG, w.data);
        }
    }
    static DWORD lastPlayers = 0;
    if (g_thread && now - lastPlayers >= 1000)
    {
        lastPlayers = now;
        std::vector<PlayerInfo> ps = g_session.players();
        std::string t = g_session.isHost() ? "Hosting" : "Connected";
        char b[96]; sprintf_s(b, " - %u player(s). Commands: help, players, resync all|<name>, say <text>\n", (unsigned)ps.size());
        t += b;
        for (size_t i = 0; i < ps.size(); ++i)
        {
            sprintf_s(b, "#%d ", (int)ps[i].id);
            t += b + ps[i].name + " (" + ps[i].faction + ")" + (ps[i].id == g_session.localId() ? " - you" : "") + "   ";
        }
        CsLock l;
        g_playersText = t;
    }
}

void logs_openConsole()
{
    if (!g_csReady) return;
    if (!g_thread) { g_thread = CreateThread(NULL, 0, windowThread, NULL, 0, NULL); return; }
    InterlockedExchange(&g_showRequest, 1);
}

bool logs_popCommand(std::string& out)
{
    if (!g_csReady) return false;
    CsLock l;
    if (g_commands.empty()) return false;
    out = g_commands.front();
    g_commands.pop_front();
    return true;
}

// The console shows the main loop's answers too.
void logs_consoleLine(const std::string& s) { if (!g_csReady) return; CsLock l; ringPush(timestamp() + s); }

} // namespace kmp
