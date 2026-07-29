// Minimal standalone repro: does pwsh.exe -NoLogo -NoExit under ConPTY exit
// immediately outside of Notepad++? Mirrors TerminalPanel::launchShell/startProcess.
// Compile: g++ -std=c++17 -o conpty_noexit_test.exe conpty_noexit_test.cpp -luser32 -lkernel32
#include <windows.h>
#include <cstdio>
#include <thread>
#include <atomic>
#include <string>
#include <chrono>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole s_pfnCreate = nullptr;
static PFN_ClosePseudoConsole  s_pfnClose  = nullptr;

static std::string g_output;
static std::atomic<bool> g_done{false};
static HANDLE g_outRead = nullptr;

void readerThread() {
    char buf[4096]; DWORD bytesRead;
    while (!g_done) {
        if (!ReadFile(g_outRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) || bytesRead == 0) {
            printf("[reader] ReadFile failed/EOF, err=%lu\n", GetLastError());
            break;
        }
        g_output.append(buf, bytesRead);
    }
}

int main() {
    printf("=== ConPTY pwsh -NoLogo -NoExit standalone repro ===\n");

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    s_pfnCreate = (PFN_CreatePseudoConsole)GetProcAddress(hK32, "CreatePseudoConsole");
    s_pfnClose  = (PFN_ClosePseudoConsole)GetProcAddress(hK32, "ClosePseudoConsole");
    if (!s_pfnCreate || !s_pfnClose) { printf("FAIL: ConPTY not available\n"); return 1; }

    HANDLE inRead, inWrite, outRead, outWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    CreatePipe(&inRead, &inWrite, &sa, 0);
    CreatePipe(&outRead, &outWrite, &sa, 0);

    HPCON hPC = nullptr;
    COORD size = { 120, 30 };
    HRESULT hr = s_pfnCreate(size, inRead, outWrite, 0, &hPC);
    if (FAILED(hr)) { printf("FAIL: CreatePseudoConsole 0x%08X\n", hr); return 1; }
    printf("PASS: ConPTY created\n");

    g_outRead = outRead;
    std::thread reader(readerThread);

    std::wstring cmdLine = L"cmd.exe";
    wchar_t cmdBuf[1024] = {};
    wcscpy_s(cmdBuf, cmdLine.c_str());

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(hPC), nullptr, nullptr);

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmdBuf, nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr, L"C:\\Users\\Water_Zhong", &siEx.StartupInfo, &pi);

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!ok) { printf("FAIL: CreateProcess err=%lu\n", GetLastError()); return 1; }
    printf("PASS: PowerShell launched PID=%lu\n", pi.dwProcessId);
    CloseHandle(pi.hThread);

    // Close console-side endpoints in THIS process, per MS docs, right after CreateProcess.
    CloseHandle(inRead);
    CloseHandle(outWrite);

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 5000);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        printf("RESULT: process EXITED on its own within 5s, exitCode=%lu\n", exitCode);
    } else {
        printf("RESULT: process STILL RUNNING after 5s (wait=%lu) -- terminating now\n", waitResult);
        TerminateProcess(pi.hProcess, 0);
    }

    g_done = true;
    CloseHandle(pi.hProcess);
    Sleep(200);
    if (reader.joinable()) reader.join();

    printf("--- Captured output (%zu bytes) ---\n", g_output.size());
    for (unsigned char c : g_output) {
        if (c == 0x1B) printf("\\e");
        else if (c == '\r') printf("\\r");
        else if (c == '\n') printf("\\n\n");
        else putchar(c);
    }
    printf("\n--- end ---\n");

    s_pfnClose(hPC);
    CloseHandle(inWrite);
    CloseHandle(outRead);
    return 0;
}
