# 高优先级规则

## 个人信息保护
- **不得向任何公开平台/互联网发布本仓库相关个人信息**，包括但不限于：真实姓名、地址、电话、邮箱、账号密码、Token、API Key
- 如果需要提及身份信息，使用别名或花名代替

## 账号与凭证
- 任何需要公开注册/使用的账号，优先使用别名或花名
- 凭证信息（Token、API Key、密码）不得出现在公开内容中

---

# NodePlus-CODE Agent Rules

## Project Overview

This repository is a NodePlus-CODE fork of [Notepad++](https://notepad-plus-plus.org/), a Windows text editor written in C++20.
The main executable is `nodeplus-code.exe`, built from `PowerEditor/`. It embeds
[Scintilla](https://www.scintilla.org/) (`scintilla/`) and [Lexilla](https://www.scintilla.org/Lexilla.html)
(`lexilla/`) as static libraries.

---

## Build Commands

> **Default shell**: Git Bash on Windows. Do **not** use PowerShell or cmd for build/test commands.

### MinGW-w64 (MSYS2) — recommended local build
```bash
# Repository build script (from repo root in Git Bash)
./build-local.sh

# Incremental build only (faster)
cd PowerEditor/gcc
mingw32-make -j$(nproc) PREBUILD_EVENT_CMD=:

# Debug build
mingw32-make -j$(nproc) DEBUG=1 PREBUILD_EVENT_CMD=:

# Clang instead of GCC
mingw32-make -j$(nproc) CXX=clang++ PREBUILD_EVENT_CMD=:
```
Output lands in `PowerEditor/gcc/bin.gcc.x86_64/` (release) or `PowerEditor/gcc/bin.gcc.x86_64-debug/` (debug).
The script is the preferred build entry point; it also runs the repository's pre-build steps.
If linking reports `Permission denied`, close the running `nodeplus-code.exe` first.

### Visual Studio 2022 (MSVC)
```bash
# From Git Bash (requires VS 2022 + MSBuild on PATH)
msbuild PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64
```
Or open `PowerEditor/visual.net/notepadPlus.sln` in the IDE and build normally.

### Skip CI for trivial commits
Add `[force none]` to the commit message to skip AppVeyor CI when only touching
comments, docs, or a single non-critical file.

---

## Test Commands

Tests run from Git Bash unless a PowerShell command is shown. A built executable
must be available at the location expected by the test launcher; this fork's
normal output is `PowerEditor/gcc/bin.gcc.x86_64/nodeplus-code.exe`.

### XML Validator (Python)
```bash
# Validate all XML config/theme/autocomplete files
python PowerEditor/Test/xmlValidator/validator_xml.py
# Dependencies (install once)
pip install requests rfc3987 pywin32 lxml
```

### FunctionList Parser Tests (PowerShell)
```powershell
# Run ALL language parser tests
cd PowerEditor/Test/FunctionList
.\localUnitestLauncher.ps1        # copies required fixtures, then runs all

# Run a SINGLE language test (e.g. python)
cd PowerEditor/Test/FunctionList
.\unitTest.ps1 python python      # args: <relative-dir> <lang-name>

# Run a UDL-based test (e.g. udl-NppExec)
.\unitTest.ps1 udl-NppExec NppExec
```
Each language dir under `FunctionList/` contains `unitTest` (input) and
`unitTest.expected.result` (expected JSON). The script invokes the built binary
with `-export=functionList` and diffs the output.

### URL Detection Tests (PowerShell)
```powershell
cd PowerEditor/Test/UrlDetection
.\verifyUrlDetection.ps1
```

### Terminal Tests

Terminal test artifacts and smoke-test helpers are under `PowerEditor/Test/Terminal/`.
Use the repository build first, then run the specific helper documented in that
directory. Do not treat generated `test_report.txt` or `test_results.json` as
source files unless the task explicitly asks to update test expectations.

### Lint and Static Checks

There is no separate project-wide lint command. The MinGW build is the primary
compile check and enables `-Wall -Wextra -Wconversion -Wpedantic`. Treat new
warnings as regressions. For XML-only changes, run the XML validator. For a
focused source check, build after editing only the affected target; the makefile
will compile the changed translation units.

### Running One Test

For FunctionList, run one language test rather than the full suite:

```powershell
cd PowerEditor/Test/FunctionList
.\unitTest.ps1 <relative-dir> <language-name>
# Example:
.\unitTest.ps1 python python
```

For URL detection, the checked-in launcher is suite-oriented; narrow it only if
the script exposes a target parameter in the local version. Always inspect the
script before inventing command-line arguments.

---

## Code Style Guidelines

Upstream coding style is defined in `CONTRIBUTING.md`. Key rules follow.

### Braces & Indentation
- **Allman style** (opening brace on its own line) for function bodies, `if`, `for`, `while`, `switch`, `class`, `struct`.
- **Exception**: one-liner method definitions in `.h` files and `try { }` blocks use K&R (opening brace same line).
- **Tabs** for indentation (display width 4 spaces, but tab character in source).
- No trailing whitespace; no reformatting unrelated lines.

### Spacing
- One space before and after every binary/ternary operator: `a == b`, `x ? y : z`.
- One space after `;` in `for` statements: `for (int i = 0; i < n; ++i)`.
- No space between function name and `(`: `foo()`, `obj.bar(x)`.
- One space between keyword and `(`: `if (cond)`, `while (true)`, `switch (val)`.

### Naming Conventions
| Entity | Convention | Example |
|---|---|---|
| Class / struct | PascalCase | `FindReplaceDlg` |
| Method / function | camelCase | `getFileNameAt` |
| Method parameter | camelCase | `myVeryLongParam` |
| Member variable | `_camelCase` (underscore prefix) | `_pEditView` |
| Global constant / enum value | UPPER_SNAKE or PascalCase enum | `IDM_VIEW_OPEN_TERMINAL` |
| Resource IDs | `IDM_`, `IDC_`, `IDD_`, `IDI_` prefixes | `IDM_FILE_NEW` |

### Types & Casts
- Use `static_cast<T>`, `reinterpret_cast<T>`, `const_cast<T>` — never C-style `(T)x`.
- Prefer references over raw pointers; use `std::unique_ptr` when heap ownership is needed; avoid `shared_ptr`.
- Use `constexpr` over `const` when value is known at compile time.
- Initialize all local and global variables. Primitives with `=`; objects with `{}`.
- Use curly-brace initialization for objects: `MyClass obj{arg}` not `MyClass obj(arg)`.
- Use `empty()` to test empty strings, not `str == ""` or `str.length() == 0`.

### C++ Best Practices
- **C++20** features are preferred; the GCC makefile compiles with `-std=c++20`.
- Use `++i` (pre-increment) rather than `i++` for iterators and loop counters.
- Do not place `using namespace` directives in header files.
- Avoid magic numbers — define named `constexpr` or `enum` values instead.
- Prefer `enum class` over plain `enum` for new enumerations.
- Use `[[fallthrough]]` for intentional switch fall-throughs.
- Use `!`, `&&`, `||` — not the ISO keywords `not`, `and`, `or`.
- Avoid Yoda conditions: `x == 42` not `42 == x`.

### Comments
- Use `//` line comments throughout, including multi-line blocks.
- Block `/* */` comments are not used in this codebase.

### Headers & Includes
- Use `#pragma once` instead of include guards.
- System/Win32 headers before local project headers.
- Do not add new third-party dependencies without strong justification.
- Include the narrowest header that provides a declaration; avoid transitive include reliance.
- Keep include ordering stable and do not reorder unrelated includes.

### Formatting and Patches

- Preserve existing CRLF/LF conventions in files being edited.
- Use ASCII by default; retain existing Unicode only when required by UI text or data.
- Do not run broad formatters over the repository.
- Keep patches focused and avoid drive-by renames or whitespace churn.
- Use `apply_patch` for small hand-written edits; use project generators only for generated assets.

### Ownership and Error Handling

- Prefer RAII and clear ownership. Match every Win32 handle/resource acquisition with its existing cleanup convention.
- Check Win32, HRESULT, and process-creation failures before using returned handles or buffers.
- Return early on invalid inputs where that matches surrounding code; do not hide failures with empty catches.
- Preserve UI-thread affinity for window operations and process UI notifications.
- For asynchronous or callback code, ensure object lifetime and shutdown ordering are explicit.
- Avoid logging credentials, user paths containing personal information, tokens, or environment secrets.

### Windows and UI Changes

- Maintain compatibility with both MSVC v143 and MinGW-w64 GCC.
- Use wide Win32 APIs consistently with surrounding code (`CreateWindowW`, `ShellExecuteW`, etc.).
- Audit resource IDs before adding one; never renumber existing IDs.
- Update `.rc`, resource headers, localization strings, and dark-mode assets together when adding UI resources.
- Test both normal and dark UI paths when changing icons, menus, dialogs, or tab controls.
- Keep ordinary document-buffer behavior separate from special views such as terminal tabs.

### Resource IDs (`resource.h`)
- Never delete or renumber existing resource IDs — all existing references must stay valid.
- New IDs must not collide with existing values; audit `resource.h` before adding.

### Win32 / Compiler Compatibility
- All Win32 API calls must compile cleanly under both MSVC (v143) and MinGW-w64 GCC.
- The GCC build uses `-Wall -Wextra -Wconversion -Wpedantic`; fix all new warnings.

---

## Pre-Commit Checklist

1. Search the relevant files for existing similar implementations before writing new code.
2. Run the appropriate tests for the area you changed:
   - XML config changes → `python PowerEditor/Test/xmlValidator/validator_xml.py`
   - FunctionList parser changes → `.\localUnitestLauncher.ps1` (PowerShell, from `PowerEditor/Test/FunctionList/`)
   - New FunctionList parser → **must** include a `unitTest` + `unitTest.expected.result` in the corresponding `FunctionList/<lang>/` directory.
3. Confirm the build succeeds locally (`./build-local.sh` or MSBuild).
4. Keep PRs to a single feature or bug fix; do not reformat unrelated code.
5. Attach every PR to a GitHub issue; new enhancements need an `Accepted` label before merging.

## OpenCode Terminal Guidance

- Use the external Windows Terminal option (`IDM_VIEW_OPEN_TERMINAL_WT`) to run OpenCode.
- Do not rely on the embedded Terminal panel for OpenCode or other advanced full-screen TUIs.
- The embedded panel uses a partial hand-written VT parser and is suitable for basic shell commands,
  but it does not yet provide complete OpenTUI-compatible terminal emulation.
- OpenCode support in the embedded panel requires a future mature VT parser and terminal screen
  model; adding individual escape-sequence replies is not considered a complete fix.
