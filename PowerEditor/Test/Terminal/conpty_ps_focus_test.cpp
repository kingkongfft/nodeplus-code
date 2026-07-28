// conpty_ps_focus_test.cpp — standalone test for ConPTY + PowerShell
// Uses a reader thread (like TerminalPanel does) for reliable ConPTY I/O.
// Tests: clean startup, keyboard input, Ctrl+C, paste, working directory
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <cassert>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole  pfnCreatePseudoConsole = nullptr;
static PFN_ResizePseudoConsole  pfnResizePseudoConsole = nullptr;
static PFN_ClosePseudoConsole   pfnClosePseudoConsole = nullptr;

static int g_pass = 0, g_fail = 0;

#define TEST(name, expr) do { \
    bool ok = (expr); \
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name); \
    if (ok) g_pass++; else { g_fail++; printf("    !!! %s\n", #expr); } \
} while(0)

#define TEST_STR(name, haystack, needle) do { \
    bool ok = ((haystack).find(needle) != std::string::npos); \
    printf("  [%s] %s", ok ? "PASS" : "FAIL", name); \
    if (!ok) { g_fail++; printf("\n    needle: '%s'\n    in:     '%.200s'", needle, (haystack).c_str()); } \
    else g_pass++; \
    printf("\n"); \
} while(0)

#define TEST_STR_NOT(name, haystack, needle) do { \
    bool ok = ((haystack).find(needle) == std::string::npos); \
    printf("  [%s] %s", ok ? "PASS" : "FAIL", name); \
    if (!ok) { g_fail++; printf("\n    found: '%s'\n    in:    '%.200s'", needle, (haystack).c_str()); } \
    else g_pass++; \
    printf("\n"); \
} while(0)

// === Resolve PowerShell ===
static std::wstring resolvePowerShell()
{
    wchar_t buf[512] = {};
    HKEY hKey;

    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\pwsh.exe",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(buf);
        ::RegGetValueW(hKey, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, buf, &sz);
        ::RegCloseKey(hKey);
    }
    if (buf[0] == L'\0') {
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell",
                0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            DWORD sz = sizeof(buf);
            ::RegGetValueW(hKey, nullptr, L"Path", RRF_RT_REG_SZ, nullptr, buf, &sz);
            ::RegCloseKey(hKey);
        }
    }
    if (buf[0] == L'\0') wcscpy_s(buf, L"powershell.exe");
    return buf;
}

// === Strip ANSI ===
static std::string stripAnsi(const std::string& s)
{
    std::string r;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\x1B' && i + 1 < s.size() && s[i+1] == '[') {
            i += 2;
            while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == 'h' || s[i] == 'l')) i++;
            if (i < s.size()) i++;
        } else if (s[i] == '\x1B' && i + 1 < s.size()) {
            i += 2;
        } else {
            r += s[i++];
        }
    }
    return r;
}

// === wstring to string ===
static std::string ws2s(const std::wstring& ws)
{
    if (ws.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

// === Check for PS prompt ===
static bool hasPSPrompt(const std::string& s) {
    return s.find("PS ") != std::string::npos;
}

// Globals for reader thread
static std::string g_outputBuf;
static std::atomic<bool> g_running{false};
static HANDLE g_outputHandle = nullptr;

static void readerThread()
{
    char buf[4096];
    DWORD bytesRead;
    int readCount = 0;
    while (g_running) {
        DWORD available = 0;
        if (::PeekNamedPipe(g_outputHandle, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            if (::ReadFile(g_outputHandle, buf, (DWORD)(sizeof(buf) - 1 < available ? sizeof(buf) - 1 : available), &bytesRead, nullptr) && bytesRead > 0) {
                buf[bytesRead] = '\0';
                g_outputBuf += buf;
                readCount++;
                printf("  [reader#%d] got %lu bytes\n", readCount, bytesRead);
            }
        }
        ::Sleep(50);
    }
    printf("  [reader] thread exited\n");
}

int main()
{
    printf("\n========================================\n");
    printf("  ConPTY PowerShell Test (reader thread)\n");
    printf("========================================\n\n");

    // === Load ConPTY ===
    printf("[1] Loading ConPTY API\n");
    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    pfnCreatePseudoConsole = (PFN_CreatePseudoConsole)::GetProcAddress(hKernel32, "CreatePseudoConsole");
    pfnResizePseudoConsole = (PFN_ResizePseudoConsole)::GetProcAddress(hKernel32, "ResizePseudoConsole");
    pfnClosePseudoConsole  = (PFN_ClosePseudoConsole)::GetProcAddress(hKernel32, "ClosePseudoConsole");
    TEST("CreatePseudoConsole", pfnCreatePseudoConsole != nullptr);
    TEST("ResizePseudoConsole", pfnResizePseudoConsole != nullptr);
    TEST("ClosePseudoConsole",  pfnClosePseudoConsole != nullptr);
    if (!pfnCreatePseudoConsole) { printf("FATAL\n"); return 1; }

    // === Resolve PowerShell ===
    printf("\n[2] Resolving PowerShell\n");
    std::wstring psPathW = resolvePowerShell();
    printf("  Path: %s\n", ws2s(psPathW).c_str());
    TEST("Path not empty", !psPathW.empty());

    // === Create ConPTY ===
    printf("\n[3] Creating ConPTY\n");
    HANDLE inPipeRead, inPipeWrite;
    HANDLE outPipeRead, outPipeWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    ::CreatePipe(&inPipeRead, &inPipeWrite, &sa, 0);
    ::CreatePipe(&outPipeRead, &outPipeWrite, &sa, 0);

    HPCON hPC = nullptr;
    HRESULT hr = pfnCreatePseudoConsole({120, 40}, inPipeRead, outPipeWrite, 0, &hPC);
    TEST("ConPTY created", SUCCEEDED(hr) && hPC != nullptr);
    if (!hPC) return 1;

    // === Start PowerShell ===
    printf("\n[4] Starting PowerShell (-NoLogo -NoExit)\n");
    wchar_t cmdLine[4096];
    wsprintfW(cmdLine, L"\"%s\" -NoLogo -NoExit", psPathW.c_str());
    printf("  Command: %s\n", ws2s(cmdLine).c_str());

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)::HeapAlloc(::GetProcessHeap(), 0, attrSize);
    ::InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize);
    ::UpdateProcThreadAttribute(siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON), nullptr, nullptr);

    PROCESS_INFORMATION pi = {};
    // Use inherited environment — TERM=xterm-256color can confuse PSReadLine
    BOOL ok = ::CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &siEx.StartupInfo, &pi);
    DWORD createErr = ::GetLastError();
    printf("  CreateProcess: ok=%d err=%lu pid=%lu\n", ok, createErr, pi.dwProcessId);
    TEST("Process started", ok);
    if (!ok) { printf("  Error: %lu\n", ::GetLastError()); return 1; }

    ::CloseHandle(pi.hThread);
    ::DeleteProcThreadAttributeList(siEx.lpAttributeList);
    ::HeapFree(::GetProcessHeap(), 0, siEx.lpAttributeList);

    // === Start reader thread ===
    printf("\n[5] Starting reader thread\n");
    g_outputBuf.clear();
    g_outputHandle = outPipeRead;
    g_running = true;
    std::thread reader(readerThread);

    // Wait for initial output — longer delay for PowerShell 7 cold start
    printf("  Waiting 4s for PS init...\n");
    ::Sleep(4000);
    printf("  outputBuf: %zu bytes\n", g_outputBuf.size());

    // Send enter to nudge PS to produce a prompt (PSReadLine may buffer)
    { DWORD _w; ::WriteFile(inPipeWrite, "\r\n", 2, &_w, nullptr); }
    ::Sleep(500);
    printf("  After enter: %zu bytes\n", g_outputBuf.size());

    // === Test A: keyboard input ===
    printf("\n[6] Keyboard input: echo HELLO_WORLD\n");
    {
        DWORD written;
        ::WriteFile(inPipeWrite, "echo HELLO_WORLD\r\n", 17, &written, nullptr);
    }
    ::Sleep(1000);
    printf("  Output: %.300s\n", g_outputBuf.c_str());
    TEST_STR("Contains HELLO_WORLD", g_outputBuf, "HELLO_WORLD");

    // === Test B: Ctrl+C ===
    printf("\n[7] Ctrl+C test (^C)\n");
    {
        DWORD written;
        char ctrlC = '\x03';
        ::WriteFile(inPipeWrite, &ctrlC, 1, &written, nullptr);
    }
    ::Sleep(500);
    printf("  After ^C: %.200s\n", g_outputBuf.c_str() + (g_outputBuf.size() > 200 ? g_outputBuf.size() - 200 : 0));

    // === Test C: paste ===
    printf("\n[8] Paste: Write-Output 'PASTE_OK'\n");
    {
        DWORD written;
        ::WriteFile(inPipeWrite, "Write-Output 'PASTE_OK'\r\n", 24, &written, nullptr);
    }
    ::Sleep(800);
    TEST_STR("Contains PASTE_OK", g_outputBuf, "PASTE_OK");

    // === Test D: working directory ===
    printf("\n[9] Working directory\n");
    {
        DWORD written;
        ::WriteFile(inPipeWrite, "(Get-Location).Path\r\n", 22, &written, nullptr);
    }
    ::Sleep(500);
    TEST_STR("Contains a path separator", g_outputBuf, "\\");

    // === Startup check ===
    printf("\n[10] Startup output quality\n");
    std::string stripped = stripAnsi(g_outputBuf);
    printf("  Stripped: %.300s\n", stripped.c_str());
    TEST_STR("Shows PS prompt", stripped, "PS ");
    TEST_STR_NOT("No pwsh.exe path garbage", stripped, "pwsh.exe");
    TEST_STR_NOT("No copyright banner", stripped, "Copyright");

    // === Cleanup ===
    printf("\n[11] Cleanup\n");
    { DWORD _w; ::WriteFile(inPipeWrite, "exit\r\n", 5, &_w, nullptr); }
    ::Sleep(300);
    g_running = false;
    pfnClosePseudoConsole(hPC);
    ::CloseHandle(inPipeWrite);
    if (reader.joinable()) reader.join();
    ::CloseHandle(outPipeRead);
    ::TerminateProcess(pi.hProcess, 0);
    ::CloseHandle(pi.hProcess);

    // === SUMMARY ===
    int total = g_pass + g_fail;
    printf("\n========================================\n");
    printf("  %d / %d passed", g_pass, total);
    if (g_fail > 0) printf("  (%d FAILED)", g_fail);
    printf("\n========================================\n");
    return g_fail > 0 ? 1 : 0;
}
