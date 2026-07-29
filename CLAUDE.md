# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **⚠️ 先看 AGENTS.md**：高优先级规则（隐私保护、凭证安全）在 `AGENTS.md` 中，请优先遵守。

## Project Overview

**NodePlus-CODE** is a fork of [Notepad++](https://notepad-plus-plus.org/) with an embedded Windows ConPTY terminal panel. It is a Windows text editor written in C++20 using pure Win32 API, Scintilla for text editing, and Lexilla for syntax highlighting.

The main executable is `nodeplus-code.exe`, built from `PowerEditor/`. It embeds Scintilla (`scintilla/`) and Lexilla (`lexilla/`) as static libraries, and Boost.Regex (`boostregex/`) for PCRE support.

## Build Systems

### MinGW-w64 (MSYS2) — primary local build
```bash
# From repo root in Git Bash:
./build-local.sh              # incremental release build
./build-local.sh --clean      # full clean release build
./build-local.sh --debug      # incremental debug build
```
- Requires MinGW-w64 at `/c/msys64/mingw64/bin`
- The script generates the library version header, then calls `mingw32-make` in `PowerEditor/gcc/`
- Output: `PowerEditor/gcc/bin.gcc.x86_64/nodeplus-code.exe` (release) or `...-debug/` (debug)
- Build log: `.build_temp/build.log`
- If linking reports `Permission denied`, close the running `nodeplus-code.exe` first

### Visual Studio 2022 (MSVC)
```bash
msbuild PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64
```
Or open `PowerEditor/visual.net/notepadPlus.sln` in VS 2022 17.5+ (v143 toolset).

### CMake
```bash
cd PowerEditor/src && mkdir _build && cd _build
cmake -G "Visual Studio 18 2026" -A x64 ..
cmake --build . --config Release
```

### NMake (Scintilla/Lexilla standalone)
```bash
cd scintilla/win32 && nmake -f scintilla.mak
cd lexilla/src && nmake -f lexilla.mak
```

## Test Commands

### FunctionList Parser Tests (PowerShell)
```powershell
cd PowerEditor/Test/FunctionList
.\localUnitestLauncher.ps1          # Run ALL language parser tests
.\unitTest.ps1 python python        # Run a SINGLE language test
.\unitTest.ps1 udl-NppExec NppExec  # Run a UDL-based test
```

### URL Detection Tests (PowerShell)
```powershell
cd PowerEditor/Test/UrlDetection
.\verifyUrlDetection.ps1
```

### XML Validation (Python)
```bash
python PowerEditor/Test/xmlValidator/validator_xml.py
```

### Terminal Tests
```powershell
cd PowerEditor/Test/Terminal
.\test_terminal.ps1   # Terminal smoke tests
```
Test artifacts: `test_report.txt`, `test_results.json`.

### Lint
No separate lint command. The MinGW build uses `-Wall -Wextra -Wconversion -Wpedantic`. Treat new warnings as regressions.

## Project Structure

```
nodeplus-code/
├── PowerEditor/
│   ├── src/                    # Main application source
│   │   ├── winmain.cpp         # Entry point
│   │   ├── Notepad_plus.h/.cpp # Core controller — owns panels, tabs, views
│   │   ├── Notepad_plus_Window.h/.cpp  # Main frame window
│   │   ├── Parameters.h/.cpp   # NppParameters — config/settings management
│   │   ├── NppBigSwitch.cpp    # Main message dispatch (WM_* + NPPM_*)
│   │   ├── NppCommands.cpp     # Menu command handlers (including terminal)
│   │   ├── NppIO.cpp           # File I/O
│   │   ├── NppNotification.cpp # Scintilla notification handling
│   │   ├── NppDarkMode.h/.cpp  # Dark mode theming
│   │   ├── WinControls/
│   │   │   ├── TerminalPanel/  # *** FORK FEATURE: ConPTY embedded terminal ***
│   │   │   │   ├── TerminalPanel.h/.cpp  # Full terminal emulator
│   │   │   │   ├── TerminalPanel.rc / _rc.h
│   │   │   │   └── BUGFIX_CONPTY_BLANK_POWERSHELL.md
│   │   │   ├── DockingWnd/     # Docking framework
│   │   │   ├── FileBrowser/    # *** Modified: CMD here removed, PowerShell here added ***
│   │   │   ├── FunctionList/   # Function list panel + parsers
│   │   │   ├── ProjectPanel/   # Project panel
│   │   │   ├── DocumentMap/    # Minimap
│   │   │   ├── ClipboardHistory/
│   │   │   └── Preference/     # Preferences dialog
│   │   ├── ScintillaComponent/ # Scintilla integration
│   │   │   ├── ScintillaEditView.h/.cpp
│   │   │   ├── Buffer.h/.cpp
│   │   │   ├── FindReplaceDlg.h/.cpp
│   │   │   └── DocTabView.h/.cpp
│   │   └── MISC/
│   │       ├── PluginsManager/ # Plugin loading/messaging
│   │       └── Common/         # Shared utilities
│   ├── gcc/                    # GCC makefile
│   ├── visual.net/             # Visual Studio solution
│   └── Test/                   # Tests
├── scintilla/                  # Scintilla editor component (fork)
├── lexilla/                    # Lexilla lexer library (fork)
└── boostregex/                 # Boost.Regex for PCRE support
```

## Architecture

### Application Flow
1. `winmain.cpp` — WinMain entry: parses command line, initializes Scintilla/Lexilla, creates main window
2. `Notepad_plus_Window` — Top-level window, manages menus/accelerators/tray
3. `Notepad_plus` — Core class owning editor UI, splitter container, find/replace, plugins, terminal panel
4. `NppBigSwitch` — Window procedure dispatching WM_* and NPPM_* messages

### Terminal Panel (Fork Feature)
The embedded terminal uses Windows ConPTY API (`CreatePseudoConsole`), with architecture:

```
Menu -> Terminal submenu -> launchTerminal(shellPath, workingDir)
  |                              |
  v                              v
TerminalPanel (docked bottom panel or main-area tab host)
  |
  +-- ConPTY: CreatePseudoConsole -> ResizePseudoConsole -> ClosePseudoConsole
  +-- Reader thread: ReadFile(_ptyOutput) -> UTF-8 decode -> ANSI parser -> render
  +-- Keyboard: WriteFile(_ptyInput) <-- WH_KEYBOARD_LL hook (focus-aware)
  +-- Alternate screen support (DECSET 1049) for TUI apps
```

Key classes:
- **`TerminalPanel`** — Full terminal emulator (docked panel, `DockingDlgInterface`)
  - ConPTY lifecycle: `initConPty()` → `createPseudoConsole()` → `startProcess()` → reader/watch threads
  - ANSI escape parser: handles SGR, cursor movement, erase, DECSET/RST, DSR, DA1/DA2, XTVERSION
  - Color support: 16-color palette, 256-color (`38;5;N`), truecolor (`38;2;R;G;B`)
  - UTF-8 incremental decoder (handles multi-byte sequences across ReadFile chunks)
  - Alternate screen buffer for full-screen TUI apps (opencode, vim, htop)
  - Keyboard hook (`WH_KEYBOARD_LL`, focus-aware — only active when terminal has focus)
  - Clipboard copy/paste with line-ending normalization, bracketed paste (`DECSET 2004`)
  - Always-dark terminal colors (independent of Notepad++ dark/light mode)
  - Debug logging: `npp_terminal_debug.log` next to `nodeplus-code.exe` (hard-coded on; `NPP_TERMINAL_DEBUG=1` opt-in alternative)

- **`_pTerminalPanel`** — Singleton docked bottom panel (one instance)
- **`_terminalTabs`** — `std::vector<TerminalPanel*>` for workspace tab instances in main document area
- **`_activeTerminalTab`** — Index tracking which workspace terminal tab is active

### Menu Commands
Terminal submenu (View > Open Terminal or Terminal popup):
| ID | Shell | Type |
|----|-------|------|
| `IDM_VIEW_OPEN_TERMINAL_PS` | PowerShell 7 (pwsh.exe), fallback PS 5.1 | Embedded ConPTY |
| `IDM_VIEW_OPEN_TERMINAL_CMD` | cmd.exe | Embedded ConPTY |
| `IDM_VIEW_OPEN_TERMINAL_GITBASH` | Git Bash via registry/path detection | Embedded ConPTY |
| `IDM_VIEW_OPEN_TERMINAL_WT` | Windows Terminal (wt.exe) | External process |

### Shell Detection (in `NppCommands.cpp`)
- **PowerShell 7**: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\pwsh.exe`
- **PowerShell 5.1**: `HKLM\SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell`
- **Git Bash**: MSYS2 (`C:\msys64\usr\bin\bash.exe`) → Git for Windows (registry) → `C:\Program Files\Git\bin\bash.exe`
- **Windows Terminal**: `wt.exe` (must be installed separately)

### Workspace Terminal Tabs
`PowerShell here` in workspace context menu (`FileBrowser`) creates terminal tabs in the main document tab area. Each tab is an independent `TerminalPanel` with its own:
- ConPTY instance and PowerShell process
- Working directory (the selected workspace folder)
- ANSI parser state, scrollback buffer, cursor position
- Selection and clipboard state

Terminal tabs coexist with file tabs in the main document tab bar. Tab selection dispatches between Scintilla views and terminal panel views.

### File Browser Changes (vs upstream)
- Removed `CMD here` from all workspace context menus
- `PowerShell here` opens an embedded terminal tab (not external process)
- `Windows Terminal here` added to workspace menus (launches external `wt.exe`)

### Application Identity
The executable and window identify as `NodePlus-CODE` (renamed from Notepad++). This includes:
- Window class/title
- Executable file description and product name (VS_VERSION_INFO)
- Default binary name: `nodeplus-code.exe`

### Plugin System
- Plugins are DLLs loaded from `plugins/` directory
- Communication via `NPPM_*` messages (`MISC/PluginsManager/Notepad_plus_msgs.h`)
- Plugin interface in `MISC/PluginsManager/PluginInterface.h`

### Scintilla Integration
- `ScintillaEditView` wraps Scintilla window, provides editor operations
- `Buffer` represents a document with state (encoding, language, modifications)
- Multiple editors can view the same buffer (split view)
- Uses Lexilla for syntax highlighting

### Configuration
- `NppParameters` (`Parameters.h/.cpp`) manages all settings
- Config files are XML: `config.xml`, `stylers.xml`, `langs.xml`, `shortcuts.xml`, etc.

### Dark Mode
- `NppDarkMode` provides dark mode theming for all UI elements
- `DarkMode/DarkMode.h` contains Win32 dark mode hooks (UAH title bar)
- Check `NppDarkMode.isEnabled()` before applying dark styling
- **Note**: Terminal panel is ALWAYS dark regardless of this setting

## Debugging

### Terminal Debug Logging
Terminal output is logged to `npp_terminal_debug.log` next to the executable. Logging is hard-coded ON (controlled by `TERM_LOG_HARDCODED_ENABLED` in `TerminalPanel.cpp`).

Logs include:
- ConPTY creation/resize/close
- Process launch and exit codes
- Raw PTY output (ANSI sequences decoded)
- Keyboard input transmission
- Clipboard operations
- Shutdown sequencing

### Resource ID Ranges
| Range | Purpose |
|-------|---------|
| 9100 | `IDD_TERMINAL_PANEL` |
| 9110-9113 | Terminal context-menu commands |
| 44201-44204 | Terminal menu commands (`IDM_VIEW_OPEN_TERMINAL_*`) |

## Coding Style

### Braces & Indentation
- **Allman style** (opening brace on its own line) for functions, `if`, `for`, `while`, `switch`, `class`, `struct`
- **Exception**: one-liner method definitions in `.h` files and `try { }` blocks use K&R
- **Tabs** for indentation (display width 4 spaces)
- No trailing whitespace

### Naming
| Entity | Convention | Example |
|--------|-----------|---------|
| Class/struct | PascalCase | `TerminalPanel` |
| Method | camelCase | `launchTerminal` |
| Method parameter | camelCase | `shellPath` |
| Member variable | `_camelCase` | `_pTerminalPanel`, `_hTermWnd` |
| Constants/enums | UPPER_SNAKE or PascalCase | `IDM_VIEW_OPEN_TERMINAL_PS` |
| Resource IDs | `IDD_`, `IDM_`, `IDC_` prefixes | `IDD_TERMINAL_PANEL` |

### Types & Casts
- `static_cast<T>` over C-style casts
- References over raw pointers; `std::unique_ptr` over `shared_ptr`
- `constexpr` over `const` when compile-time
- Initialize all variables; `=` for primitives, `{}` for objects
- `empty()` to test empty strings over `str == ""`

### C++ Best Practices
- C++20 features preferred; GCC makefile uses `-std=c++20`
- Pre-increment (`++i`) over post-increment
- No `using namespace` in headers
- `enum class` over plain `enum`
- `[[fallthrough]]` for intentional fall-through
- `!`, `&&`, `||` — not `not`, `and`, `or`
- Avoid Yoda conditions: `x == 42` not `42 == x`

### Comments
- `//` line comments throughout; no `/* */` style

### Headers
- `#pragma once` instead of include guards
- System/Win32 headers before project headers

### Terminal Panel Specific
- Thread safety: access to `_buffer`, cursor, selection guarded by `_bufMutex` (recursive mutex)
- Input writes guarded by `_inputMutex` (plain mutex)
- ANSI parser is stateful and called from reader thread; UI repaint triggered via `InvalidateRect` + `WM_APP_TERM_SYNCSCROLL`
- ConPTY resize debounced (400ms timer `IDT_TERM_RESIZE`) to avoid blank-screen issues during docking layout

## Pre-Commit Checklist

1. Search existing implementations before writing new code
2. Run tests for the area changed:
   - XML config → `python PowerEditor/Test/xmlValidator/validator_xml.py`
   - FunctionList → `.\localUnitestLauncher.ps1`
   - Terminal → `.\test_terminal.ps1`
3. Confirm build succeeds locally (`./build-local.sh` or MSBuild)
4. Keep PRs to a single feature/bug-fix; no unrelated reformatting

## OpenCode/TUI Terminal Guidance

- Use the external `Windows Terminal` option (`IDM_VIEW_OPEN_TERMINAL_WT`) for OpenCode or other advanced full-screen TUIs
- The embedded panel supports basic OpenTUI operation (alternate screen, DA1/DA2, DSR, DECRQM, XTVERSION, OSC capability queries, Kitty keyboard protocol)
- Mouse reporting, DECRQSS, and comprehensive VT sequence support are not complete
