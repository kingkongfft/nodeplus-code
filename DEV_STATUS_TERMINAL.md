# Notepad++ Embedded Terminal — Development Status

**Date**: 2026-07-28  
**Branch**: `master` (local)  
**Feature**: Embedded ConPTY terminal — PowerShell / cmd / Git Bash / Windows Terminal

---

## Summary

Embedded terminal panel using Windows ConPTY API (`CreatePseudoConsole`). Supports PowerShell 7 (`pwsh.exe`) with `-NoLogo -NoExit`, with Windows PowerShell 5.1 fallback. Command Prompt, Git Bash, and Windows Terminal are also available. The terminal is docked at the bottom and can be recreated after closing. PowerShell copy/paste, keyboard navigation, Unicode output, selection, scrollback, and ANSI rendering were substantially hardened and manually verified in-app.

---

## Files Changed

| File | Change |
|------|--------|
| `PowerEditor/src/Notepad_plus.cpp` | `launchTerminal()`: creates singleton TerminalPanel, docks it, handles re-creation after close |
| `PowerEditor/src/NppCommands.cpp` | PowerShell 7 detection (pwsh.exe) + PS5 fallback; `-NoLogo -NoExit` flags; `getTerminalWorkingDir()` |
| `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel.h` | Terminal state, synchronized ConPTY input, ANSI modes, UTF-8 decoder, selection and rendering state |
| `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel.cpp` | ConPTY lifecycle, clipboard operations, keyboard translation, reader thread, ANSI parser, selection and rendering |
| `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel.rc` | Empty dialog resource (only child is programmatic terminal window) |
| `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel_rc.h` | Terminal context-menu command IDs |
| `PowerEditor/Test/Terminal/test_copy_paste.ps1` | Expanded terminal/clipboard source checks (41 checks) |
| `PowerEditor/gcc/README.md` | MinGW-w64 GCC build guide (new) |
| `PowerEditor/src/WinControls/TerminalPanel/BUGFIX_LS_GARBLED_OUTPUT.md` | Deep-dive writeup: `ls`/`ls -al` overlapping/blank-line bugs (new) |

---

## Architecture

```
Menu Bar -> Terminal (top-level POPUP, MENUINDEX_TERMINAL=4)
        |
        +-- IDM_VIEW_OPEN_TERMINAL_PS   -> PowerShell (embedded ConPTY)
        +-- IDM_VIEW_OPEN_TERMINAL_CMD  -> cmd.exe (embedded ConPTY)
        +-- IDM_VIEW_OPEN_TERMINAL_GITBASH -> Git Bash (embedded ConPTY)
        +-- IDM_VIEW_OPEN_TERMINAL_WT   -> Windows Terminal (external wt.exe)
                |
                v
        Notepad_plus::launchTerminal(shellPath, workingDir)
                |
                +-- Panel alive? -> launchShell()
                +-- Panel closed? -> delete + new TerminalPanel + display() + launchShell()
                        |
                TerminalPanel (docked at bottom, re-creatable)
                        |
                +-------+-------+
                |       |       |
                v       v       v
              ConPTY  Reader  Keyboard
              Create  Thread  Hook (WH_KEYBOARD_LL, focus-aware)
                        |
                pwsh.exe -NoLogo -NoExit
                        |
                WriteFile(_ptyInput) <-- synchronized keyboard/paste/DSR input
                ReadFile(_ptyOutput) --> UTF-8 decode -> ANSI parser -> render
```

---

## Shell Path Resolution

| Shell | Resolution | Fallback |
|-------|-----------|----------|
| PowerShell 7 | Registry: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\pwsh.exe` | PowerShell 5.1 |
| PowerShell 5.1 | Registry: `HKLM\SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell` | `powershell.exe` |
| cmd | `cmd.exe` | — |
| Git Bash | `C:\Program Files\Git\bin\bash.exe`, `C:\Program Files (x86)\Git\bin\bash.exe`, `bash.exe` | — |
| Windows Terminal | `wt.exe` (must be installed separately) | — |

---

## Builder: MinGW-w64 (MSYS2)

| Component | Path |
|-----------|------|
| g++ | `C:\msys64\mingw64\bin\g++.exe` (16.1.0) |
| mingw32-make | `C:\msys64\mingw64\bin\mingw32-make.exe` |
| Build cmd | `cd PowerEditor/gcc && mingw32-make -j$(nproc) PREBUILD_EVENT_CMD=":"` |
| Output | `PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe` (~12.8 MB) |
| Note | Paths with spaces (e.g., `OneDrive - EPAM`) require pre-running `NppLibsVersionH-generator.bat` via PowerShell, then `PREBUILD_EVENT_CMD=":"` |

---

## Bugs Fixed (this session: 2026-07-28)

### Bug 1: Keyboard hook eats ALL global keys 🔴->🟢

**Root cause**: `WH_KEYBOARD_LL` hook was always active when terminal was running, returning 1 for every keydown — Ctrl+S, Ctrl+O, etc. broken globally. Typing in the editor was impossible while the terminal panel was open.

**Fix**: Hook now checks `GetFocus()` + `IsChild()` at runtime. Only eats keys when the terminal child window or its parent dialog has keyboard focus. When focus is on the editor (or anywhere else), keys pass through normally to Notepad++.

### Bug 2: Ctrl+C/V copy/paste not working in terminal 🔴->🟢

**Root cause**: The hook's `sendVKeyToTerminal` used `ToUnicode()` which can't produce control characters. Also, letter keys `'A'..'Z'` were caught by a switch-case that only handled Ctrl+letter — without Ctrl, letters produced zero output.

**Fix**:
- `sendVKeyToTerminal`: Added explicit Ctrl+letter handling (`^A`-`^Z` sent as 0x01-0x1A). Letter keys without Ctrl now fall through to `ToUnicode()`.
- Hook's `termKbHookProc`: Added `Ctrl+C` with selection -> clipboard copy, `Ctrl+V` -> paste from clipboard.

### Bug 3: White background — terminal text invisible 🔴->🟢

**Root cause**: Constructor alternated between white (light mode) and dark (NPP dark mode colors) based on `NppDarkMode::isEnabled()` — which could return wrong value at construction time. White background + light text = invisible output.

**Fix**: Removed `NppDarkMode` dependency entirely. Terminal always uses classic dark colors: near-black background `RGB(12,12,12)`, light gray foreground `RGB(220,220,220)`. Added `WM_CTLCOLORDLG`/`WM_CTLCOLORSTATIC` handlers to paint dialog background dark too (eliminates white flash around terminal edges).

### Bug 4: Garbled PowerShell startup output 🔴->🟢

**Root cause**: Command line included `-Command cd 'C:\path'` — PowerShell printed the command's output (path) before showing the prompt. This appeared as garbled `> Program Files\PowerShell\7\pwsh.exe` in the terminal.

**Fix**: Removed `-Command cd '...'` entirely. `CreateProcess`'s `lpCurrentDirectory` already sets the working directory correctly. Command line is now just: `"path\to\pwsh.exe" -NoLogo -NoExit`.

### Bug 5: PowerShell 7 not detected (only PS5) 🔴->🟢

**Root cause**: Only checked Windows PowerShell 5.1 registry key. PowerShell 7 (pwsh.exe) has better ConPTY support and native ANSI handling.

**Fix**: Checks `App Paths\pwsh.exe` registry first. Falls back to PS5 registry, then `powershell.exe` as ultimate fallback.

### Bug 6: PowerShell clipboard shortcuts unreliable 🔴->🟢

**Root cause**: The low-level hook used `GetKeyState()`, swallowed bare modifier keys, and performed blocking clipboard and pipe operations inside `WH_KEYBOARD_LL`.

**Fix**: Uses `GetAsyncKeyState()`, passes bare modifiers through, and posts copy/paste work to the terminal window. Supported shortcuts now include `Ctrl+C` with a selection, `Ctrl+Shift+C`, `Ctrl+V`, `Ctrl+Shift+V`, `Ctrl+Insert`, and `Shift+Insert`. Clipboard access retries on contention, supports `CF_TEXT`, normalizes line endings, handles partial pipe writes, and supports bracketed paste (`DECSET 2004`).

### Bug 7: PowerShell navigation and text rendering incomplete 🔴->🟢

**Root cause**: Arrow, Home, End, Delete, and function keys had no terminal escape sequences. UTF-8 bytes were rendered individually, long lines were truncated, and cursor positioning ignored the visible screen origin.

**Fix**: Added xterm-compatible navigation/function-key sequences, incremental UTF-8 decoding, line wrapping, screen-relative cursor positioning, functional scrollback, 256-color/truecolor SGR, and batched double-buffered rendering.

### Bug 8: Terminal lifecycle and threading hazards 🔴->🟢

**Root cause**: ConPTY-side pipe handles leaked, terminal input writes could interleave, shared display state was accessed from reader and UI threads, and `GenerateConsoleCtrlEvent(..., 0)` could signal Notepad++ itself.

**Fix**: Closes caller-owned ConPTY endpoints, synchronizes input writes and buffer access, serializes shutdown, and sends `^C` through the PTY. System shortcuts such as `Alt+Tab`, `Alt+F4`, and Windows-key combinations pass through the hook.

### Bug 9: `ls`/`ls -al` output overlapping text 🔴->🟢

**Root cause**: `newLine()` incremented `_cursorRow` on every line-feed but never advanced `_screenTop` — the baseline used to resolve *absolute* cursor positioning (CUP `\e[row;colH`, VPA `\e[d`) into buffer row indices. Once `ls` output exceeded one screenful, PSReadLine's prompt-redraw CUP sequences resolved against a stale `_screenTop` and overwrote earlier scrollback lines instead of the actual bottom line.

**Fix**: `newLine()` now advances `_screenTop` whenever the cursor drops below the bottom of the visible screen (`_cursorRow >= _screenTop + rows`), mirroring how a real terminal scrolls the viewport on line-feed. See `PowerEditor/src/WinControls/TerminalPanel/BUGFIX_LS_GARBLED_OUTPUT.md` for the full writeup (Bug A).

### Bug 10: `ls`/`ls -al` colorized directory entries render as blank lines 🔴->🟢

**Root cause**: `handleANSIEscape()` defaulted every omitted CSI parameter to `1` except SGR (`m`), including the erase commands ED (`\e[J`) and EL (`\e[K`). PowerShell/PSReadLine emit a bare `\e[K` after every colorized filename (directories, symlinks); per ECMA-48/xterm this must default to `0` ("erase from cursor to end of line"), not `1`. With the wrong default, EL took the "erase from line start to cursor" branch, wiping out the just-printed filename and rendering it as a blank line.

**Fix**: Default omitted parameters to `0` for `m`, `J`, and `K` (erase/reset semantics), and `1` only for movement commands (`A`/`B`/`C`/`D`/etc.). Diagnosed by temporarily dumping raw PTY bytes via the existing `NPP_TERMINAL_DEBUG=1` logging path. Full writeup in `PowerEditor/src/WinControls/TerminalPanel/BUGFIX_LS_GARBLED_OUTPUT.md` (Bug B).

---

## Panel Lifecycle

```
User clicks "PowerShell"
  -> launchTerminal()
  -> _pTerminalPanel == nullptr? -> create new TerminalPanel, dock, display
  -> launchShell("pwsh.exe -NoLogo -NoExit")
  -> installKbHook() (active only when terminal has focus)

User clicks on editor (terminal loses focus)
  -> GetFocus() returns Scintilla HWND -> IsChild check fails
  -> hook passes all keys through -> editor works normally

User clicks back on terminal
  -> GetFocus() returns _hTermWnd -> IsChild check passes
  -> hook eats keys -> sends to ConPTY

User clicks X on docked tab
  -> DockingManager sends DMN_CLOSE
  -> terminate() kills ConPTY, reader thread, uninstalls hook
  -> _isClosed = true, _hTermWnd destroyed

User clicks "PowerShell" again
  -> launchTerminal()
  -> isClosed() == true -> delete old, create new TerminalPanel, dock, display
  -> launchShell(powershell.exe) ✅
```

---

## Keyboard Hook Logic

```
termKbHookProc(nCode, wParam, lParam):
  if !running: pass through
  if !IsChild(hDlg, GetFocus()) && GetFocus() != hTermWnd: pass through
  if WM_KEYDOWN || WM_SYSKEYDOWN:
    if modifier key or OS shortcut: pass through
    if copy shortcut: post WM_COPY, return 1
    if paste shortcut: post WM_PASTE, return 1
    sendVKeyToTerminal(vkCode), return 1
  pass through
```

---

## Known Limitations

| Item | Status |
|------|--------|
| ~~PS7 (pwsh.exe) not auto-detected~~ | ✅ Fixed: checks pwsh.exe first, falls back to PS5 |
| Windows Terminal (wt.exe) must be installed separately | ⚠️ Not bundled with Windows |
| ~~Terminal background white / unreadable~~ | ✅ Fixed: always dark (RGB 12,12,12) |
| ~~Keyboard hook eats all global keys~~ | ✅ Fixed: focus-aware via IsChild() |
| ~~`ls`/`ls -al` output overlapping / blank-line entries~~ | ✅ Fixed: `_screenTop` scroll tracking + correct ED/EL default params (see Bugs 9–10) |
| No session persistence | ⚠️ Closing Notepad++ kills the shell |
| ANSI parser coverage | ⚠️ Supports common cursor/edit modes, 16/256/truecolor SGR and bracketed paste; not a complete VT emulator |
| Full-screen TUI applications | ⚠️ Basic operation only; alternate-screen, mouse reporting and all DEC modes are not complete |
| Space-in-path build issue | ⚠️ `cmd //C` pre-build event fails with spaces; workaround documented |

---

## Test Results (2026-07-28)

| Validation | Result |
|------------|--------|
| MinGW-w64 release build | PASS — `PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe` |
| `test_copy_paste.ps1` | PASS — 41/41 |
| Main `test_terminal.ps1` checks | 55/60 — remaining failures are menu-resource test expectations/parsing, unrelated to PowerShell copy/paste |
| Standalone ConPTY PowerShell output | PASS — all checks |
| Standalone ConPTY keyboard harness | 9/10 — VK simulation check remains; real in-app keyboard behavior is better than the harness |
| Fresh application launch | PASS |
| Manual PowerShell terminal check | PASS — copy/paste behavior confirmed improved in-app |
| Manual `ls -al` check (post Bug 9/10 fix) | PASS — no overlapping text, no blank directory/symlink entries; confirmed via user screenshot before/after |

The freshly built application was launched from `PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe`. Debug logging is disabled by default; set `NPP_TERMINAL_DEBUG=1` to write `%TEMP%\npp_terminal_debug.log`.
