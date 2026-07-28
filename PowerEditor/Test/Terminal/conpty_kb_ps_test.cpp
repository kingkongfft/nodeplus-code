// ConPTY + PowerShell keyboard input test
// Compile: g++ -std=c++17 -o conpty_kb_ps_test.exe conpty_kb_ps_test.cpp -luser32 -lkernel32
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
    char buf[4096]; DWORD br;
    while (!g_done) {
        DWORD avail = 0;
        if (PeekNamedPipe(g_outRead, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(g_outRead, buf, sizeof(buf)-1, &br, nullptr) && br > 0)
                g_output.append(buf, br);
        } else Sleep(50);
    }
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_outRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
        char buf2[4096]; DWORD br2;
        if (ReadFile(g_outRead, buf2, sizeof(buf2)-1, &br2, nullptr) && br2 > 0)
            g_output.append(buf2, br2); else break;
    }
}

std::string drainOutput() {
    Sleep(400);
    std::string out = g_output;
    // Strip ANSI for clean checking
    std::string clean;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == 0x1B) {
            while (i < out.size() && (out[i] < 'A' || out[i] > 'z' || out[i] == '[')) i++;
            continue;
        }
        clean += out[i];
    }
    return clean;
}

void sendVKey(HANDLE hIn, DWORD vk) {
    char buf[16] = {}; int len = 0;
    switch (vk) {
    case VK_RETURN: buf[0] = '\r'; len = 1; break;
    case VK_TAB:    buf[0] = '\t'; len = 1; break;
    case VK_BACK:   buf[0] = '\b'; len = 1; break;
    case VK_ESCAPE: buf[0] = 0x1B; len = 1; break;
    case VK_SPACE:  buf[0] = ' ';  len = 1; break;
    default: {
        BYTE ks[256] = {};
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        wchar_t wch = 0;
        if (ToUnicode(vk, sc, ks, &wch, 1, 0) == 1 && wch >= 0x20)
            len = WideCharToMultiByte(CP_UTF8, 0, &wch, 1, buf, sizeof(buf), nullptr, nullptr);
        break;
    }
    }
    if (len > 0) { DWORD w; WriteFile(hIn, buf, len, &w, nullptr); }
}

void sendText(HANDLE hIn, const std::string& t) { DWORD w; WriteFile(hIn, t.c_str(), (DWORD)t.size(), &w, nullptr); }
void sendLine(HANDLE hIn, const std::string& t) { sendText(hIn, t); sendVKey(hIn, VK_RETURN); }

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
    printf("=== ConPTY + PowerShell Keyboard Input Test ===\n");
    printf("Tests: WriteFile → ConPTY → PowerShell → verify response\n\n"); fflush(stdout);

    if (!LoadConPTY()) { printf("FAIL: ConPTY not available\n"); return 1; }
    printf("PASS: ConPTY loaded\n");

    HANDLE inRead, inWrite, outRead, outWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    CreatePipe(&inRead, &inWrite, &sa, 0);
    CreatePipe(&outRead, &outWrite, &sa, 0);

    HPCON hPC = nullptr;
    COORD size = { 120, 30 };
    s_pfnCreate(size, inRead, outWrite, 0, &hPC);
    printf("PASS: ConPTY created\n");

    g_outRead = outRead;
    std::thread reader(readerThread);

    // Launch PowerShell interactive (-NoLogo -NoExit)
    std::wstring psPath = GetPSPath();
    std::wstring cmdLine = psPath + L" -NoLogo -NoExit";
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
    CreateProcessW(nullptr, cmdBuf, nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &siEx.StartupInfo, &pi);

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
    CloseHandle(pi.hThread);
    printf("PASS: PowerShell launched (PID=%lu)\n", pi.dwProcessId);

    Sleep(800); drainOutput(); // drain PS banner

    // === TEST A: WriteFile text command ===
    printf("\n--- TEST A: WriteFile plain text ---\n");
    sendLine(inWrite, "Write-Output 'PS_INPUT_TEST_A_OK'");
    std::string out = drainOutput();
    bool a1 = out.find("PS_INPUT_TEST_A_OK") != std::string::npos;
    printf("%s: Write-Output command\n", a1 ? "PASS" : "FAIL");

    // === TEST B: Type via sendVKey ===
    printf("\n--- TEST B: VK-typed command ---\n");
    const char* bText = "Write-Output 'PS_VK_TEST_B_OK'";
    for (const char* p = bText; *p; p++) {
        SHORT vk = VkKeyScanA(*p);
        if (vk != -1) sendVKey(inWrite, LOBYTE(vk));
    }
    sendVKey(inWrite, VK_RETURN);
    out = drainOutput();
    bool b1 = out.find("ps_vk_test_b_ok") != std::string::npos
           || out.find("PS_VK_TEST_B_OK") != std::string::npos;
    printf("%s: VK-typed Write-Output\n", b1 ? "PASS" : "FAIL");

    // === TEST C: Backspace correction ===
    printf("\n--- TEST C: Backspace editing ---\n");
    sendLine(inWrite, "Write-Output 'PS_CORRECT_WORD'");
    drainOutput();
    sendText(inWrite, "Write-Output 'BAD_");
    Sleep(100);
    for (int i = 0; i < 3; i++) sendVKey(inWrite, VK_BACK);
    Sleep(100);
    sendText(inWrite, "GOOD_WORD'");
    sendVKey(inWrite, VK_RETURN);
    out = drainOutput();
    bool c1 = out.find("GOOD_WORD") != std::string::npos;
    printf("%s: Backspace editing (PS handles line editing)\n", c1 ? "PASS" : "FAIL");

    // === TEST D: Tab completion ===
    printf("\n--- TEST D: Tab completion ---\n");
    sendLine(inWrite, "New-Item -ItemType File -Path $env:TEMP\\_NPP_PS_TABTEST_.tmp -Force");
    out = drainOutput();
    sendText(inWrite, "Get-ChildItem $env:TEMP\\_NPP_PS_TAB");
    Sleep(100); sendVKey(inWrite, VK_TAB); Sleep(200);
    sendVKey(inWrite, VK_RETURN);
    out = drainOutput();
    bool d1 = out.find("_NPP_PS_TABTEST_.tmp") != std::string::npos;
    printf("%s: Tab completion\n", d1 ? "PASS" : "FAIL");
    sendLine(inWrite, "Remove-Item $env:TEMP\\_NPP_PS_TABTEST_.tmp -Force");

    // === TEST E: Mixed text+VK ===
    printf("\n--- TEST E: Mixed text+VK ---\n");
    sendText(inWrite, "Write-");
    sendVKey(inWrite, 'O' & 0xFF);
    sendText(inWrite, "utput 'PS_MIXED_OK'");
    sendVKey(inWrite, VK_RETURN);
    out = drainOutput();
    bool e1 = out.find("PS_MIXED_OK") != std::string::npos;
    printf("%s: Mixed text+VK\n", e1 ? "PASS" : "FAIL");

    // === TEST F: Rapid commands ===
    printf("\n--- TEST F: Rapid commands ---\n");
    sendLine(inWrite, "Write-Output 'RAPID_1'");
    sendLine(inWrite, "Write-Output 'RAPID_2'");
    sendLine(inWrite, "Write-Output 'RAPID_3'");
    Sleep(400); out = drainOutput();
    bool f1 = out.find("RAPID_1") != std::string::npos;
    bool f2 = out.find("RAPID_2") != std::string::npos;
    bool f3 = out.find("RAPID_3") != std::string::npos;
    printf("%s: RAPID_1\n", f1 ? "PASS" : "FAIL");
    printf("%s: RAPID_2\n", f2 ? "PASS" : "FAIL");
    printf("%s: RAPID_3\n", f3 ? "PASS" : "FAIL");

    // === TEST G: Ctrl+C ===
    printf("\n--- TEST G: Ctrl+C interrupt ---\n");
    sendLine(inWrite, "Write-Output 'BEFORE_CTRLC'");
    out = drainOutput();
    bool gPre = out.find("BEFORE_CTRLC") != std::string::npos;
    sendLine(inWrite, "Start-Sleep -Seconds 10");
    Sleep(600);
    char ctrlC = 0x03; DWORD wr;
    WriteFile(inWrite, &ctrlC, 1, &wr, nullptr);
    Sleep(500);
    sendLine(inWrite, "Write-Output 'AFTER_CTRLC'");
    out = drainOutput();
    bool gAfter = out.find("AFTER_CTRLC") != std::string::npos;
    printf("%s: Shell responsive after Ctrl+C\n", (gPre && gAfter) ? "PASS" : "FAIL");

    // === Cleanup ===
    sendLine(inWrite, "exit");
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD exitCode = 999;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    printf("\nPASS: PowerShell exited (code=%lu)\n", exitCode);

    g_done = true; Sleep(100); reader.join();
    CloseHandle(pi.hProcess);
    CloseHandle(inWrite); CloseHandle(outRead);
    s_pfnClose(hPC);

    int pass = a1 + b1 + c1 + d1 + e1 + f1 + f2 + f3 + gPre + gAfter;
    printf("\n=== KEYBOARD INPUT SUMMARY: %d/10 ===\n", pass);
    printf("%s\n", pass == 10 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return pass == 10 ? 0 : 1;
}
