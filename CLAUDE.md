# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Notepad++ is a free, open-source source code editor and Notepad replacement for Windows, written in C++20 using pure Win32 API. It uses Scintilla for text editing and Lexilla for syntax highlighting/lexing.

## Build Systems

### Visual Studio (primary)
- Solution: `PowerEditor/visual.net/notepadPlus.sln`
- Requires: Visual Studio 2022 17.5+ (v143 toolset)
- Configurations: Debug | Release × Win32 | x64 | ARM64
- Build: Open solution in VS or use `msbuild PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64`

### GCC/MinGW-w64
- Makefile: `PowerEditor/gcc/makefile`
- Requires: MSYS2 with MinGW-w64 GCC
- Build: `cd PowerEditor/gcc && mingw32-make` (add `DEBUG=1` for debug, `VERBOSE=1` for verbose)
- Output in `bin.gcc.<arch>/` (e.g., `bin.gcc.x86_64/`)
- Supports: GCC and Clang compilers

### CMake
- Source: `PowerEditor/src/CMakeLists.txt`
- Requires: Pre-built Scintilla and Lexilla (via nmake), then:
  ```
  cd PowerEditor/src && mkdir _build && cd _build
  cmake -G "Visual Studio 18 2026" -A x64 ..
  cmake --build . --config Release
  ```

### NMake (scintilla/lexilla standalone)
```
cd scintilla/win32 && nmake -f scintilla.mak
cd lexilla/src && nmake -f lexilla.mak
```

## Build Dependencies
- Notepad++ links Scintilla (`libscintilla.lib`) and Lexilla (`liblexilla.lib`) as static libraries
- Uses Boost.Regex (PCRE) included in `boostregex/` — stripped down to needed files
- Windows SDK libraries: comctl32, crypt32, dbghelp, ole32, sensapi, shlwapi, uuid, uxtheme, version, wininet, wintrust, dwmapi, bcrypt, imm32, msimg32

## Project Structure

```
notepad-plus-plus/
├── PowerEditor/
│   ├── src/                    # Main application source
│   │   ├── winmain.cpp         # Entry point
│   │   ├── Notepad_plus.h/.cpp # Core controller class
│   │   ├── Notepad_plus_Window.h/.cpp  # Main window
│   │   ├── Parameters.h/.cpp   # NppParameters — config management
│   │   ├── NppBigSwitch.cpp    # Main message dispatch
│   │   ├── NppCommands.cpp     # Command implementations
│   │   ├── NppIO.cpp           # File I/O
│   │   ├── NppNotification.cpp # Scintilla notification handling
│   │   ├── NppDarkMode.h/.cpp  # Dark mode support
│   │   ├── ScintillaComponent/ # Scintilla integration
│   │   │   ├── ScintillaEditView.h/.cpp  # Main editor view wrapper
│   │   │   ├── Buffer.h/.cpp   # Document buffer management
│   │   │   ├── FindReplaceDlg.h/.cpp # Find/Replace dialog
│   │   │   ├── AutoCompletion.h/.cpp
│   │   │   ├── SmartHighlighter.h/.cpp
│   │   │   ├── DocTabView.h/.cpp    # Document tabs
│   │   │   └── Printer.h/.cpp  # Printing support
│   │   ├── WinControls/        # Windows controls
│   │   │   ├── DockingWnd/     # Docking framework
│   │   │   ├── TabBar/         # Modern tab bar
│   │   │   ├── ToolBar/        # Customizable toolbar
│   │   │   ├── FileBrowser/    # File explorer panel
│   │   │   ├── FunctionList/   # Function list panel + parsers
│   │   │   ├── ProjectPanel/   # Project panel
│   │   │   ├── DocumentMap/    # Document map/minimap
│   │   │   ├── Preference/     # Preferences dialog
│   │   │   ├── PluginsAdmin/   # Plugins admin dialog
│   │   │   └── ClipboardHistory/
│   │   ├── MISC/
│   │   │   ├── PluginsManager/ # Plugin loading/messaging
│   │   │   ├── RegExt/         # Regex extension dialog
│   │   │   ├── Process/        # Process launching
│   │   │   ├── Exception/      # Crash handling (MiniDumper)
│   │   │   └── Common/         # Shared utilities
│   │   ├── DarkMode/           # Dark mode hooks
│   │   ├── uchardet/           # Encoding detection
│   │   ├── json/               # JSON library (3rd-party)
│   │   ├── pugixml/            # XML parser (3rd-party)
│   │   └── resource.h          # Version info and resource IDs
│   ├── gcc/                    # GCC makefile
│   ├── visual.net/             # Visual Studio solution
│   ├── installer/              # Themes, localization, config files
│   ├── Test/                   # Tests
│   │   ├── FunctionList/       # Per-language parser tests (PowerShell)
│   │   ├── UrlDetection/       # URL detection tests (Lua)
│   │   └── xmlValidator/       # XML validation (Python)
│   └── bin/                    # Win32 output
├── scintilla/                  # Scintilla editor component (fork)
├── lexilla/                    # Lexilla lexer library (fork)
└── boostregex/                 # Boost.Regex for PCRE support
```

## Architecture Notes

### Application Flow
1. `winmain.cpp` — WinMain entry: parses command line, initializes Scintilla/Lexilla, creates main window
2. `Notepad_plus_Window` — Top-level window, manages menus/accelerators/tray
3. `Notepad_plus` — Core application class that owns the editing UI, splitter container, find/replace, plugins
4. `NppBigSwitch` — Window procedure that dispatches WM_* and NPPM_* messages

### Plugin System
- Plugins are DLLs loaded from the `plugins/` directory
- Communication via `NPPM_*` messages defined in `MISC/PluginsManager/Notepad_plus_msgs.h`
- Plugin interface defined in `MISC/PluginsManager/PluginInterface.h`
- Scintilla direct access via `SCI_*` messages

### Scintilla Integration
- `ScintillaEditView` wraps Scintilla window, provides methods for editor operations
- `Buffer` represents a single document with its state (encoding, language, modifications)
- Multiple editors can view the same buffer (split view)
- Uses Lexilla for syntax highlighting

### Configuration
- `NppParameters` (in `Parameters.h/.cpp`) manages all settings
- Config files are XML: `config.xml`, `stylers.xml`, `langs.xml`, `shortcuts.xml`, etc.
- `NppXml` handles XML serialization

### Dark Mode
- `NppDarkMode` provides dark mode theming for all UI elements
- `DarkMode/DarkMode.h` contains Win32 dark mode hooks (UAH title bar, etc.)
- Check `NppDarkMode.isEnabled()` before applying dark styling

## Testing

### FunctionList Tests (PowerShell)
```powershell
cd PowerEditor/Test/FunctionList
.\unitTestLauncher.ps1   # Run all function list tests
```
- Per-language tests in `PowerEditor/Test/FunctionList/<lang>/`
- Each contains `unitTest` (input) and `unitTest.expected.result` files
- Runs Notepad++ with `-export=functionList -l<lang>` flag

### UrlDetection Tests (Lua)
```powershell
cd PowerEditor/Test/UrlDetection
.\verifyUrlDetection.ps1
```

### XML Validation (Python)
```powershell
python PowerEditor/Test/xmlValidator/validator_xml.py
```
Validates XML config files against schemas.

## Coding Style

- **Braces**: K&R style — braces on new line for functions/classes, Java-style only for single-line method definitions in headers and try-catch
- **Indentation**: Tabs (displayed as 4 spaces)
- **Naming**: Classes PascalCase, methods camelCase, member variables prefixed with `_`
- **Language**: C++20, prefer modern features (constexpr, auto, {} initialization)
- **Types**: Prefer C++ casts (`static_cast<>`) over C-style casts
- **Pointers**: Prefer references, use `unique_ptr` over `shared_ptr`
- **Headers**: No `using namespace` in headers
- **Comments**: Use `//` C++ style, not `/* */` C style
- See `CONTRIBUTING.md` for full coding style rules

## CI

GitHub Actions in `.github/workflows/CI_build.yml`:
- **before_build**: Smart job that detects changed file types and selects appropriate matrix
- **build_windows**: MSVC builds all platforms/configurations
- **build_windows_cmake**: CMake-based x64 Release build
- **build_windows_msys2**: GCC (MINGW32/MINGW64) and Clang (CLANG64) builds
- Commit messages can use tags: `[force all]`, `[force one]`, `[force xml]`, `[force none]`
