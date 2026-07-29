# 高优先级规则

## 个人信息保护
- **不得向任何公开平台/互联网发布本仓库相关个人信息**，包括但不限于：真实姓名、地址、电话、邮箱、账号密码、Token、API Key
- 如果需要提及身份信息，使用别名或花名代替

## 账号与凭证
- 任何需要公开注册/使用的账号，优先使用别名或花名
- 凭证信息（Token、API Key、密码）不得出现在公开内容中

---

# Notepad++ Agent Rules

## Project Overview

This is a fork of [Notepad++](https://notepad-plus-plus.org/), a Windows text editor written in C++20.
The main executable is `nodeplus-code.exe`, built from `PowerEditor/`. It embeds
[Scintilla](https://www.scintilla.org/) (`scintilla/`) and [Lexilla](https://www.scintilla.org/Lexilla.html)
(`lexilla/`) as static libraries.

---

## Build Commands

> **Default shell**: Git Bash on Windows. Do **not** use PowerShell or cmd for build/test commands.

### MinGW-w64 (MSYS2) — recommended local build
```bash
# Full clean build (from repo root in Git Bash)
./build-local.sh

# Incremental build only (faster)
cd PowerEditor/gcc
mingw32-make -j$(nproc) PREBUILD_EVENT_CMD=:

# Debug build
mingw32-make -j$(nproc) DEBUG=1 PREBUILD_EVENT_CMD=:

# Clang instead of GCC
mingw32-make -j$(nproc) CXX=clang++ PREBUILD_EVENT_CMD=:
```
Output lands in `PowerEditor/gcc/bin.gcc.x86_64/` (release) or `bin.gcc.x86_64-debug/` (debug).

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

All tests run from Git Bash (or PowerShell where noted). A built `notepad++.exe`
must exist at `PowerEditor/bin/` before running FunctionList or UrlDetection tests.

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
