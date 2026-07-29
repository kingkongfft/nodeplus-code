# Bug Fix: Blank PowerShell Terminal Under ConPTY

**Date**: 2026-07-29
**File**: `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel.cpp`

---

## Symptom

The embedded terminal panel opened with a black, empty display. `pwsh.exe -NoLogo -NoExit` was
launched successfully (ConPTY created, `CreateProcessW` returned success, process got a PID), but
either exited immediately with code 0, or never printed a prompt — leaving the panel permanently
blank.

Debug log evidence:

```text
[TERM] createPseudoConsole: CreatePseudoConsole hr=0x00000000 hPC=...
[TERM] startProcess: CreateProcess result=1 err=0 pid=12460
[TERM] readerThread: started, reading from PTY...
[TERM] RAW[16]: \e[?9001h\e[?1004h
[TERM] RAW[140]: \e[?25l\e[2J\e[m\e[H\r\n\r\n\r\n...(blank lines)...\e[H\e]0;pwsh.exe\x07\e[?25h
[TERM] watchProcessExit: shell process exited, exitCode=0 (0x00000000)
```

ConPTY and process creation both succeeded. Only startup control sequences arrived; no prompt text.

---

## Investigation

### What was ruled out

1. **Application killing the shell** — `terminate()` / `DMN_CLOSE` / `WM_DESTROY` do not appear in
   the log before `watchProcessExit`, so the panel did not kill the shell during docking or
   lifecycle.
2. **Plain `CreateProcess` (no ConPTY)** — a standalone test confirmed `pwsh.exe -NoLogo -NoExit`
   stays alive for 12+ seconds when launched without ConPTY. PowerShell itself is healthy.
3. **Pipe-handle lifetime** — closing `_ptyInputReadSide` / `_ptyOutputWriteSide` immediately
   after `CreateProcess` (per Microsoft docs recommendation) was trialled. The blank-terminal
   symptom was unchanged.
4. **Endpoint-security / AV termination** — no relevant crash events found in the Windows
   Application event log.

### Isolation test

A minimal standalone C++ harness was created:
`PowerEditor/Test/Terminal/conpty_noexit_test.cpp`

This mirrors the exact `createPseudoConsole` → `startProcess` flow from `TerminalPanel.cpp`.
When the harness did **not** set `STARTF_USESTDHANDLES`, `pwsh.exe` exited within 5 seconds.
When `STARTF_USESTDHANDLES` was added and the ConPTY pipe endpoints were bound as standard
handles, `pwsh.exe` stayed alive for the full 5-second wait — confirming the root cause.

---

## Root Cause

`CreateProcessW` was called with `EXTENDED_STARTUPINFO_PRESENT` but **without**
`STARTF_USESTDHANDLES` in `StartupInfo.dwFlags`. The ConPTY pipe endpoints were therefore
**not wired as the hosted process's standard input/output/error**.

The pseudoconsole was created and attached, but the shell could not reliably connect its stdio
streams to the PTY channel. This caused the blank display and early exit with code 0.

---

## Fix

In `TerminalPanel::startProcess()`, before `CreateProcessW`, configure the standard handle
fields of `STARTUPINFOEXW`:

```cpp
siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
siEx.StartupInfo.hStdInput  = _ptyInputReadSide;   // ConPTY input read-side
siEx.StartupInfo.hStdOutput = _ptyOutputWriteSide; // ConPTY output write-side
siEx.StartupInfo.hStdError  = _ptyOutputWriteSide;
```

The console-side pipe endpoints (`_ptyInputReadSide`, `_ptyOutputWriteSide`) must be kept open
until `CreateProcessW` completes and are closed during `terminate()`.

**Diff summary** (`TerminalPanel.cpp`, `startProcess` function):

```diff
  STARTUPINFOEXW siEx = {};
  siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
+ siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
+ siEx.StartupInfo.hStdInput = _ptyInputReadSide;
+ siEx.StartupInfo.hStdOutput = _ptyOutputWriteSide;
+ siEx.StartupInfo.hStdError = _ptyOutputWriteSide;
```

---

## Validation

1. **Standalone ConPTY test** (`conpty_noexit_test.cpp`) — after adding `STARTF_USESTDHANDLES`,
   `pwsh.exe -NoLogo -NoExit` remained alive beyond the 5-second timeout. Before the fix it
   exited within 5 seconds.

2. **Rebuilt application** — `mingw32-make binary -j$(nproc)` succeeded (warnings only, no
   errors).

3. **In-app verification** — terminal log after launching from the rebuilt binary:

```text
[TERM] RAW[25]: PS C:\Users\Water_Zhong>
```

   The PowerShell prompt appeared. The terminal panel is no longer blank.
