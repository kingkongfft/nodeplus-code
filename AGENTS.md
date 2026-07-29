# AGENTS.md

## Red rules (never violate)

- NEVER run `git commit` or `git push` unless the user explicitly asks for it in the current request.
- NEVER commit or push secrets: API keys, tokens, passwords, private keys, `.env` files, or credential files. Warn the user instead if such a change is requested.
- NEVER hardcode secrets, keys, or passwords into source, config, or docs.

## Scope and priorities

- This file applies to the whole repository.
- Notepad++ is a Windows desktop application written primarily in C++20 and Win32.
- `PowerEditor/src/` is the application; `scintilla/` and `lexilla/` are bundled upstream-derived components.
- Follow the style of the component and nearby code before applying the general rules below.
- Read `CONTRIBUTING.md` before substantial work; its requirements are authoritative.
- Keep changes compact. Do not reformat unrelated code, normalize line endings, or perform drive-by refactors.
- Preserve user changes in a dirty worktree and never edit generated build outputs.
- No `.cursorrules`, `.cursor/rules/`, or `.github/copilot-instructions.md` exists at this writing.

## Toolchain

- Primary toolchain: Visual Studio 2022 17.5+ with MSVC v143 and a Windows SDK.
- Main solution: `PowerEditor/visual.net/notepadPlus.sln`.
- Configurations are `Debug` or `Release`; platforms are `Win32`, `x64`, or `ARM64`.
- MSVC uses C++20, warning level 4, conformance mode, SDL checks, and warnings as errors.
- MinGW builds require MSYS2, MinGW-w64 GCC/Clang, and `mingw32-make`.
- CMake is secondary and requires Scintilla and Lexilla libraries to be built first.
- Test scripts expect PowerShell, Python 3, and in some cases a prepared `PowerEditor/bin/` tree.

## Build commands

Run commands from the repository root unless a command explicitly changes directory.

```powershell
# Recommended fast local build matching the smallest CI job
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Debug /p:platform=Win32

# Typical release build
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Release /p:platform=x64

# Other supported examples
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Debug /p:platform=x64
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Release /p:platform=ARM64
```

For the CMake route, use a Visual Studio developer shell:

```powershell
nmake /f scintilla\win32\scintilla.mak
nmake /f lexilla\src\lexilla.mak
cmake -S PowerEditor\src -B PowerEditor\src\_build -G "Visual Studio 18 2026" -A x64
cmake --build PowerEditor\src\_build --config Release
```

For MSYS2/MinGW, invoke the repository makefile from the root:

```bash
mingw32-make -f PowerEditor/gcc/makefile
mingw32-make -f PowerEditor/gcc/makefile DEBUG=1
mingw32-make -f PowerEditor/gcc/makefile CXX=clang++
mingw32-make -f PowerEditor/gcc/makefile CXX=clang++ CLANGANALYZE=1
```

- Add `VERBOSE=1` to expose compiler commands.
- Output directories are named `bin.<compiler>.<arch>` with `-debug` for debug binaries.
- The makefile enables `-Wpedantic -Wall -Wextra -Wconversion`; fix new warnings.

## Active local build method: MinGW-w64 (MSYS2)

VS 2022 is installed but lacks the "Desktop development with C++" workload. Use MSYS2/MinGW-w64 for local builds.

**Environment:**
- MSYS2: `C:/msys64`
- g++: `C:/msys64/mingw64/bin/g++.exe`
- mingw32-make: `C:/msys64/mingw64/bin/mingw32-make.exe`

**Output:** `PowerEditor/gcc/bin.gcc.x86_64/nodeplus-code.exe` (~13 MB)

**Component versions (current):**
- Scintilla 5.6.4 / Lexilla 5.5.1 / Boost Regex 1_90

**Useful flags:**

| Flag | Effect |
|------|--------|
| `DEBUG=1` | Debug build |
| `VERBOSE=1` | Verbose output |
| `-j$(nproc)` | Parallel build (all CPU cores) |

### Path-with-spaces workaround (required when repo is under a path containing spaces)

The MinGW makefile passes `$(CURDIR)` to sub-makes without quoting. If the repo path contains spaces (e.g. `OneDrive - EPAM`), the `all` target fails when building Scintilla/Lexilla. Use the two-step procedure below instead.

**Step 1 — generate the version header** (run once per clean, from repo root, Git Bash or cmd):

```bash
cmd /C "PowerEditor\\src\\NppLibsVersionH-generator.bat"
```

**Step 2 — build from within `PowerEditor/gcc/`** (relative paths avoid the space issue):

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd PowerEditor/gcc
mingw32-make -j$(nproc)
# or for the binary target only (Scintilla/Lexilla already built):
mingw32-make binary -j$(nproc)
```

Running through MSYS2 bash explicitly also works:

```bash
"C:/msys64/usr/bin/bash.exe" -lc '
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
  cd "$(cygpath -u "<REPO_ROOT>/PowerEditor/gcc")"
  mingw32-make -j$(nproc)
'
```

**Clean build:**

```bash
# From repo root (Git Bash / cmd)
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
mingw32-make -f PowerEditor/gcc/makefile clean

# Then follow the two-step build above
```

> If the repo root path does **not** contain spaces the standard one-liner still works:
> ```bash
> export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
> mingw32-make -f PowerEditor/gcc/makefile -j$(nproc)
> ```

**VS build** (once C++ workload is installed — run from Git Bash/MSYS2):

```bash
MSYS_NO_PATHCONV=1 "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
  PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64
```

> `MSYS_NO_PATHCONV=1` prevents Git Bash/MSYS2 from converting `/p:` flags to paths.

## Lint and validation

- There is no repository-wide formatter or standalone lint command; a clean warning-free build is the C++ lint gate.
- Do not run a formatter across existing files. Formatting-only PRs are explicitly discouraged.
- MSVC Debug x64 enables native code analysis; use it when changing ownership, bounds, or Win32 code.
- Clang static analysis is available through the MinGW command with `CLANGANALYZE=1`.
- XML validation dependencies are `requests`, `rfc3987`, `pywin32`, and `lxml`.

```powershell
python -m pip install requests rfc3987 pywin32 lxml
python PowerEditor\Test\xmlValidator\validator_xml.py
```

- The XML validator scans all supported XML under APIs, function lists, native languages, themes, and source.
- It has no single-file CLI; for one edited XML file, run the full validator.

## Application tests

The Function List and URL tests require a Win32 Debug executable and runtime files in `PowerEditor/bin/`.
CI prepares that tree as follows after building:

```powershell
Copy-Item PowerEditor\visual.net\Debug\Notepad++.exe PowerEditor\bin
Copy-Item PowerEditor\src\langs.model.xml PowerEditor\bin
Copy-Item PowerEditor\src\stylers.model.xml PowerEditor\bin
Copy-Item PowerEditor\installer\functionList PowerEditor\bin -Recurse -Force
Copy-Item PowerEditor\installer\xml4Config\doLocalConf.xml PowerEditor\bin
Copy-Item PowerEditor\installer\filesForTesting\regexGlobalTest.xml PowerEditor\bin\functionList
Copy-Item PowerEditor\installer\filesForTesting\overrideMap.xml PowerEditor\bin\functionList
```

Run all Function List tests:

```powershell
Push-Location PowerEditor\Test\FunctionList
.\unitTestLauncher.ps1
Pop-Location
```

Run one Function List test, which is the preferred focused application test:

```powershell
Push-Location PowerEditor\Test\FunctionList
.\unitTest.ps1 cpp cpp
.\unitTest.ps1 python\class python
Pop-Location
```

- Arguments are `RELATIVE_TEST_DIRECTORY` and `LANGUAGE_NAME`.
- A test directory contains `unitTest` and `unitTest.expected.result`.
- The script returns `0` for pass, `1` for skipped/missing input, `-1` for mismatch, and `-2` for exception.
- Function List parser changes must add or update their matching test fixture.

Run URL detection tests after preparing `PowerEditor/bin/`:

```powershell
Push-Location PowerEditor\Test\UrlDetection
.\verifyUrlDetection.ps1
Pop-Location
```

## Component unit tests

Scintilla and Lexilla have Catch-based unit executables. On Windows use `mingw32-make`, not `nmake`.

```powershell
mingw32-make -C scintilla\test\unit test
mingw32-make -C lexilla\test\unit test
mingw32-make -C lexilla\test test
```

After the unit executable is built, use Catch's test-name filter to run one test case:

```powershell
scintilla\test\unit\unitTest.exe "Document"
lexilla\test\unit\unitTest.exe "WordList"
```

- Quote names containing spaces; list available Catch tests with `unitTest.exe --list-tests`.
- Run the narrowest relevant test first, then the full component suite, then an application build.

## C++ style

- Use tabs for indentation; tabs are conventionally displayed at width 4.
- Put opening braces on a new line for functions, classes, and control statements.
- Exceptions: a one-line inline method in a header may use same-line braces; `try {` also uses same-line braces.
- Put one space around binary and ternary operators and after semicolons in `for` clauses.
- Do not put a space between a function name and `(`; do put one after control keywords such as `if`.
- Keep `switch` cases indented, prefer a final `default`, and mark intentional fallthrough with `[[fallthrough]]`.
- Avoid magic numbers; prefer named `constexpr` values or enums.
- Prefer brace initialization for class objects and initialize every variable.
- For primitive and enum values, existing code commonly uses `=` initialization; preserve local convention.
- Prefer `constexpr` over `const` when the value is compile-time evaluable.
- Use `empty()` to test strings and containers rather than comparing with an empty literal.
- Use `static_cast`, `reinterpret_cast`, and other C++ casts; avoid C-style casts.
- Use `!`, `&&`, and `||`, not the alternative tokens `not`, `and`, and `or`.
- Prefer pre-increment (`++i`) for consistency.
- Avoid Yoda conditions.

## Includes, types, and ownership

- In `.cpp` files, include the matching header first, then platform/system, standard library, external, and project headers.
- Separate include groups with blank lines and preserve the ordering pattern of the surrounding file.
- Use `#pragma once` where that is the established header convention.
- Never place `using namespace` directives in headers.
- Use C++20 features where they improve safety or clarity while respecting component compatibility.
- Prefer references to pointers when null is not meaningful.
- Prefer automatic storage and RAII; avoid raw `new` and `delete`.
- Prefer `std::unique_ptr`; use `std::shared_ptr` only when ownership is genuinely shared.
- Use fixed-width or Win32 types when the API/serialization boundary requires their exact semantics.
- Do not replace established Win32 handles or message types merely for stylistic consistency.

## Naming and comments

- Classes and structs use PascalCase.
- Methods, functions, parameters, and local variables use camelCase.
- Prefix class and struct data members with `_`.
- Use descriptive names; do not introduce unexplained abbreviations.
- Follow established uppercase naming for resource IDs, macros, and legacy enum constants.
- Use `//` comments rather than block comments.
- Comment intent, invariants, or non-obvious Win32 behavior; do not narrate straightforward code.
- Reuse existing helpers, constants, resource IDs, and globals before creating new ones.

## Error handling and changes

- Match the surrounding API's error model: status enum, `bool`, exception, Win32 error, or user-visible message.
- Check Win32/API return values when failure is actionable; preserve error details needed by callers or users.
- Use RAII for handles and resources where local abstractions support it, and guarantee cleanup on every exit path.
- Catch specific exceptions when recovery or translation is possible; do not silently swallow failures.
- Avoid broad `catch (...)` unless at a process/UI boundary where it prevents escape and reports failure.
- Validate pointers only when null is a valid or externally supplied state; otherwise express non-null with references.
- Preserve UI-thread assumptions and existing message-dispatch behavior.
- Add focused regression coverage for bug fixes; keep tests deterministic and remove generated result files.
- Build the affected platform/configuration and run the narrowest relevant tests before reporting completion.
- Do not alter vendored Scintilla/Lexilla style to match `PowerEditor`; follow each subtree's nearby code.
