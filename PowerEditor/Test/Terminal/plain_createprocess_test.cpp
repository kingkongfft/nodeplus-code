// Does a PLAIN CreateProcess (no ConPTY at all) of pwsh.exe survive?
// Isolates whether something external is killing child shell processes.
#include <windows.h>
#include <cstdio>

int main() {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    wchar_t cmdBuf[1024] = {};
    wcscpy_s(cmdBuf, L"\"C:\\Program Files\\PowerShell\\7\\pwsh.exe\" -NoLogo -NoExit -Command \"Start-Sleep -Seconds 8; Write-Output DONE\"");

    BOOL ok = CreateProcessW(nullptr, cmdBuf, nullptr, nullptr, FALSE,
        CREATE_NEW_CONSOLE, nullptr, L"C:\\Users\\Water_Zhong", &si, &pi);

    if (!ok) { printf("FAIL: CreateProcess err=%lu\n", GetLastError()); return 1; }
    printf("PASS: launched PID=%lu\n", pi.dwProcessId);
    CloseHandle(pi.hThread);

    DWORD waitResult = WaitForSingleObject(pi.hProcess, 12000);
    if (waitResult == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        printf("RESULT: exited on its own, exitCode=%lu\n", exitCode);
    } else {
        printf("RESULT: still running after 12s (survived) -- terminating now\n");
        TerminateProcess(pi.hProcess, 0);
    }
    CloseHandle(pi.hProcess);
    return 0;
}
