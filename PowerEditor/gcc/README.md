# Building Notepad++ with MinGW-w64 (GCC)

This directory contains the **MinGW-w64 GCC makefile** for building Notepad++ on Windows, as an alternative to the Visual Studio solution in `PowerEditor/visual.net/`.

---

## Prerequisites

| Component | Location |
|-----------|----------|
| **MSYS2** | [Download & Install](https://www.msys2.org/) → `C:\msys64` (default) |
| **MinGW-w64 GCC** | Installed via MSYS2: `pacman -S mingw-w64-x86_64-gcc` |
| **mingw32-make** | Included with MinGW-w64: `pacman -S mingw-w64-x86_64-make` |
| **Windows SDK** | Not required — GCC uses native Win32 headers from MinGW-w64 |

> **Note**: GCC 14+ (MSYS2) is tested. Other versions may work but are untested.

---

## Quick Start

```bash
# 1. Launch MSYS2 MinGW64 shell, or use Git Bash with PATH set:
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

# 2. Generate build version header (only needed once, or when versions change):
cd PowerEditor/gcc
powershell -Command "& '$(pwd)/../src/NppLibsVersionH-generator.bat'"

# 3. Build:
mingw32-make -j$(nproc) PREBUILD_EVENT_CMD=":"

# 4. The binary is at:
#    PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe
```

**⚠️ Paths with spaces**: If the repo path contains spaces (e.g., `OneDrive - EPAM`), the Makefile's pre-build event (`cmd //C`) fails. Workaround: run the `NppLibsVersionH-generator.bat` once via PowerShell, then pass `PREBUILD_EVENT_CMD=":"` to skip it.

---

## Build Options

| Flag | Effect |
|------|--------|
| `DEBUG=1` | Debug build (output in `bin.gcc.x86_64-debug/`) |
| `VERBOSE=1` | Print all compiler/linker commands |
| `-j$(nproc)` | Parallel build using all CPU cores |
| `TARGET_CPU=x86_64` | Force 64-bit target (multilib GCC) |
| `TARGET_CPU=i686` | Force 32-bit target (multilib GCC) |
| `CXX=clang++` | Use Clang instead of GCC |
| `CLANGANALYZE=1` | Enable Clang static analyzer (requires `CXX=clang++`) |
| `PREBUILD_EVENT_CMD=:` | Skip the pre-build event (useful when header is already generated) |

### Examples

```bash
# Debug build
mingw32-make DEBUG=1 -j$(nproc) PREBUILD_EVENT_CMD=":"

# Release build with verbose output
mingw32-make VERBOSE=1 -j$(nproc) PREBUILD_EVENT_CMD=":"

# Build with Clang
mingw32-make CXX=clang++ -j$(nproc) PREBUILD_EVENT_CMD=":"
```

---

## Output Structure

```
PowerEditor/gcc/
├── bin.gcc.x86_64/            # Release build output
│   └── notepad++.exe          # The built executable (~13 MB)
├── bin.gcc.x86_64.build/      # Intermediate object files (release)
│   ├── libscintilla.a         # Scintilla static library
│   ├── liblexilla.a           # Lexilla static library
│   └── *.o                    # Object files
├── bin.gcc.x86_64-debug/      # Debug build output (if DEBUG=1)
├── bin.gcc.x86_64.debug/      # Debug intermediate files (if DEBUG=1)
├── makefile                   # GCC build makefile
├── gcc-fixes.h                # GCC-specific compatibility fixes
├── manifest.rc                # Windows manifest resource
└── README.md                  # This file
```

---

## How It Works

1. **Scintilla and Lexilla** are built first as static libraries (`libscintilla.a`, `liblexilla.a`).
2. **Notepad++** is linked against these libraries plus Boost.Regex (PCRE) from `../../boostregex/`.
3. The **pre-build event** runs `NppLibsVersionH-generator.bat` to generate `PowerEditor/src/NppLibsVersion.h` with version strings (Scintilla, Lexilla, Boost.Regex).
4. The **post-build step** copies necessary DLLs and configuration files to the output directory.

---

## Troubleshooting

### `cmd //C` fails with spaces in path

```
'EPOS-onedrive' is not recognized as an internal or external command
```

**Fix**: Run the header generator once, then skip pre-build:
```bash
powershell -Command "& '$(pwd)/../src/NppLibsVersionH-generator.bat'"
mingw32-make -j$(nproc) PREBUILD_EVENT_CMD=":"
```

### `windows.h` not found

MinGW-w64 headers are missing. Ensure you have the full MinGW-w64 package:
```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-headers
```

### Linker errors about `WinMain`

The build expects a Windows subsystem entry point. Ensure you're using the correct MinGW-w64 toolchain (not the MSYS2 native one).

---

## Comparison with Visual Studio Build

| Aspect | MinGW-w64 (GCC) | Visual Studio (MSVC) |
|--------|-----------------|----------------------|
| Build system | `make` (Makefile) | `msbuild` (.sln/.vcxproj) |
| Compiler | GCC 16+ | MSVC v143 |
| Setup | MSYS2 + MinGW-w64 | VS 2022 + C++ workload + Windows SDK |
| Disk space | ~1 GB | ~10 GB+ |
| Debugger | GDB | Visual Studio debugger |
| Pre-build event | `.bat` via `cmd //C` | Native VS event |
| Output binary | ~13 MB | ~14 MB (Debug) |
| Performance | Comparable | Comparable |

---

## CI Reference

The GitHub Actions CI uses this build method in the `build_windows_msys2` job. See [`.github/workflows/CI_build.yml`](../../.github/workflows/CI_build.yml) for the exact flags and environment setup.
