# Build Guide

This guide covers how to build Notepad++ locally on Windows.

## Toolchain overview

| Method | Prerequisite | Status |
|--------|-------------|--------|
| MSVC (MSBuild) | VS 2022 with "Desktop development with C++" workload | Available once workload is installed |
| MinGW-w64 (MSYS2) | MSYS2 at `C:/msys64` | **Active local build method** |
| CMake | VS developer shell + Scintilla/Lexilla built first | Secondary |

---

## MinGW-w64 (MSYS2) — active local build method

### Environment

- MSYS2: `C:/msys64`
- g++: `C:/msys64/mingw64/bin/g++.exe`
- mingw32-make: `C:/msys64/mingw64/bin/mingw32-make.exe`

### Output

```
PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe  (~13 MB)
```

### Component versions (current)

- Scintilla 5.6.4 / Lexilla 5.5.1 / Boost Regex 1_90

### Useful flags

| Flag | Effect |
|------|--------|
| `DEBUG=1` | Debug build |
| `VERBOSE=1` | Expose compiler commands |
| `-j$(nproc)` | Parallel build (all CPU cores) |
| `CXX=clang++` | Build with Clang instead of GCC |
| `CLANGANALYZE=1` | Enable Clang static analysis |

---

### Normal build (repo path has no spaces)

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
mingw32-make -f PowerEditor/gcc/makefile -j$(nproc)
```

---

### Path-with-spaces workaround

> **Required when the repo lives under a path containing spaces**, e.g. `OneDrive - EPAM`.
>
> The MinGW makefile passes `$(CURDIR)` to sub-makes without quoting.
> Spaces in the path cause the Scintilla/Lexilla sub-make rules to fail.

#### Step 1 — generate the version header

Run once per clean from the repo root (Git Bash or cmd):

```bash
cmd /C "PowerEditor\\src\\NppLibsVersionH-generator.bat"
```

Expected output:

```
Scintilla version detected: "5.6.4"
Lexilla version detected: "5.5.1"
Boost Regex version detected: "1_90"
```

#### Step 2 — build from within `PowerEditor/gcc/`

Relative paths in the sub-directory avoid the spaces problem:

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd PowerEditor/gcc
mingw32-make -j$(nproc)
```

If Scintilla/Lexilla are already built (e.g. incremental rebuild), use:

```bash
mingw32-make binary -j$(nproc)
```

Alternatively, invoke through MSYS2 bash explicitly (replace `<REPO_ROOT>`):

```bash
"C:/msys64/usr/bin/bash.exe" -lc '
  export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
  cd "$(cygpath -u "<REPO_ROOT>/PowerEditor/gcc")"
  mingw32-make -j$(nproc)
'
```

---

### Clean build

```bash
# From repo root (Git Bash / cmd)
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
mingw32-make -f PowerEditor/gcc/makefile clean

# Then re-run the two-step build above (spaces workaround)
# or the normal one-liner if the path has no spaces
```

---

## MSVC (MSBuild)

Requires VS 2022 with the "Desktop development with C++" workload installed.

Run from the repository root:

```powershell
# Recommended fast local build (smallest CI job equivalent)
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Debug /p:platform=Win32

# Typical release build
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Release /p:platform=x64

# Other supported combinations
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Debug /p:platform=x64
msbuild PowerEditor\visual.net\notepadPlus.sln /m /p:configuration=Release /p:platform=ARM64
```

From Git Bash or MSYS2 (use `MSYS_NO_PATHCONV=1` to prevent path mangling):

```bash
MSYS_NO_PATHCONV=1 "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
  PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64
```

---

## CMake (secondary)

Use a Visual Studio developer shell:

```powershell
nmake /f scintilla\win32\scintilla.mak
nmake /f lexilla\src\lexilla.mak
cmake -S PowerEditor\src -B PowerEditor\src\_build -G "Visual Studio 18 2026" -A x64
cmake --build PowerEditor\src\_build --config Release
```

---

## Lint and static analysis

- A clean warning-free build is the C++ lint gate; there is no separate formatter or lint command.
- MSVC Debug x64 enables native code analysis; prefer it when changing ownership, bounds, or Win32 code.
- Clang static analysis:

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
mingw32-make -f PowerEditor/gcc/makefile CXX=clang++ CLANGANALYZE=1
```

- XML validation:

```powershell
python -m pip install requests rfc3987 pywin32 lxml
python PowerEditor\Test\xmlValidator\validator_xml.py
```

---

## Tests

### Function List and URL tests

Require a Win32 Debug executable and runtime files in `PowerEditor/bin/`. Prepare the tree:

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

Run a single Function List test:

```powershell
Push-Location PowerEditor\Test\FunctionList
.\unitTest.ps1 cpp cpp
.\unitTest.ps1 python\class python
Pop-Location
```

Exit codes: `0` pass, `1` skipped/missing input, `-1` mismatch, `-2` exception.

Run URL detection tests:

```powershell
Push-Location PowerEditor\Test\UrlDetection
.\verifyUrlDetection.ps1
Pop-Location
```

### Scintilla / Lexilla component unit tests

```bash
mingw32-make -C scintilla/test/unit test
mingw32-make -C lexilla/test/unit test
mingw32-make -C lexilla/test test
```

Run a single Catch test case by name:

```bash
scintilla/test/unit/unitTest.exe "Document"
lexilla/test/unit/unitTest.exe "WordList"
```

List available test cases:

```bash
scintilla/test/unit/unitTest.exe --list-tests
```
