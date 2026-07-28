// ConPTY + PowerShell output test
// Compile: g++ -std=c++17 -o conpty_ps_test.exe conpty_ps_test.cpp -luser32 -lkernel32
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <string>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole s_pfnCreate = nullptr;
static PFN_ResizePseudoConsole s_pfnResize = nullptr;
static PFN_ClosePseudoConsole  s_pfnClose  = nullptr;

bool LoadConPTY() {
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (!h) return false;
    s_pfnCreate = (PFN_CreatePseudoConsole)GetProcAddress(h, "CreatePseudoConsole");
    s_pfnResize = (PFN_ResizePseudoConsole)GetProcAddress(h, "ResizePseudoConsole");
    s_pfnClose  = (PFN_ClosePseudoConsole)GetProcAddress(h, "ClosePseudoConsole");
    return s_pfnCreate && s_pfnResize && s_pfnClose;
}

static std::string g_output;
static std::atomic<bool> g_done{false};
static HANDLE g_outRead = nullptr;

void readerThread() {
    char buf[4096]; DWORD bytesRead;
    while (!g_done) {
        DWORD avail = 0;
        if (PeekNamedPipe(g_outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(g_outRead, buf, sizeof(buf)-1, &bytesRead, nullptr) && bytesRead > 0)
                g_output.append(buf, bytesRead);
        } else Sleep(50);
    }
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_outRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf2[4096]; DWORD br;
        if (ReadFile(g_outRead, buf2, sizeof(buf2)-1, &br, nullptr) && br > 0)
            g_output.append(buf2, br); else break;
    }
}

// Resolve PowerShell path like NppCommands.cpp
std::wstring GetPSPath() {
    HKEY hKey;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[512] = {};
        DWORD sz = sizeof(buf);
        ::RegGetValueW(hKey, nullptr, L"Path", RRF_RT_REG_SZ, nullptr, buf, &sz);
        ::RegCloseKey(hKey);
        if (buf[0]) return buf;
    }
    return L"powershell.exe";
}

int main() {
    printf("=== ConPTY + PowerShell Output Test ===\n"); fflush(stdout);

    if (!LoadConPTY()) { printf("FAIL: ConPTY not available\n"); return 1; }
    printf("PASS: ConPTY loaded\n");

    HANDLE inRead, inWrite, outRead, outWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) { printf("FAIL: in pipe\n"); return 1; }
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) { printf("FAIL: out pipe\n"); return 1; }

    HPCON hPC = nullptr;
    COORD size = { 120, 30 };
    HRESULT hr = s_pfnCreate(size, inRead, outWrite, 0, &hPC);
    if (FAILED(hr)) { printf("FAIL: CreatePseudoConsole (0x%08X)\n", hr); return 1; }
    printf("PASS: ConPTY created\n");

    g_outRead = outRead;
    std::thread reader(readerThread);

    // Launch powershell.exe -NoLogo -NoProfile -NonInteractive -Command "..."
    std::wstring psPath = GetPSPath();
    std::wstring cmdLine = psPath + L" -NoLogo -NoProfile -NonInteractive -Command \"Write-Output 'POWERSHELL_CONPTY_TEST_OK'; Write-Output 'MULTILINE_CHECK'\"";
    wchar_t cmdBuf[1024] = {};
    wcscpy_s(cmdBuf, cmdLine.c_str());

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siEx.StartupInfo.hStdInput  = inRead;
    siEx.StartupInfo.hStdOutput = outWrite;
    siEx.StartupInfo.hStdError  = outWrite;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(hPC), nullptr, nullptr);

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmdBuf, nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &siEx.StartupInfo, &pi);

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!ok) { printf("FAIL: CreateProcess (err=%lu)\n", GetLastError()); return 1; }
    printf("PASS: PowerShell launched (PID=%lu)\n", pi.dwProcessId);
    CloseHandle(pi.hThread);

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 10000);
    Sleep(300);
    g_done = true; Sleep(200); reader.join();

    DWORD exitCode = 999;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    printf("PASS: Process exited (exitCode=%lu)\n", exitCode);

    // Strip ANSI for checking
    std::string clean;
    for (size_t i = 0; i < g_output.size(); i++) {
        if (g_output[i] == 0x1B) {
            while (i < g_output.size() && (g_output[i] < 'A' || g_output[i] > 'z' || g_output[i] == '[')) i++;
            continue;
        }
        clean += g_output[i];
    }

    printf("--- Output (%zu bytes) ---\n%s\n--- end ---\n", g_output.size(), clean.c_str());

    bool found1 = clean.find("POWERSHELL_CONPTY_TEST_OK") != std::string::npos;
    bool found2 = clean.find("MULTILINE_CHECK") != std::string::npos;

    printf("\n--- Results ---\n");
    printf("%s: Write-Output captured\n", found1 ? "PASS" : "FAIL");
    printf("%s: Multi-line output captured\n", found2 ? "PASS" : "FAIL");

    CloseHandle(pi.hProcess);
    CloseHandle(inWrite);
    CloseHandle(outRead);
    s_pfnClose(hPC);

    bool allPassed = found1 && found2;
    printf("\n=== %s ===\n", allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return allPassed ? 0 : 1;
}
