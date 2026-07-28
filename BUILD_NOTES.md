# Notepad++ Build Notes

## Environment

| Item | Value |
|------|-------|
| Machine | Windows x64 |
| MSYS2 | `C:/msys64` |
| MinGW-w64 g++ | `C:/msys64/mingw64/bin/g++.exe` |
| mingw32-make | `C:/msys64/mingw64/bin/mingw32-make.exe` |
| VS 2022 | `C:/Program Files/Microsoft Visual Studio/2022/Community/` (missing C++ workload) |

## Build: MinGW-w64 (MSYS2)

The VS 2022 build fails because the "Desktop development with C++" workload is not installed. Use MSYS2 instead.

### Command

```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd PowerEditor/gcc
mingw32-make -j$(nproc)
```

### Options

| Flag | Effect |
|------|--------|
| `DEBUG=1` | Debug build |
| `VERBOSE=1` | Verbose output |
| `-j$(nproc)` | Parallel build (all CPU cores) |

### Output

```
PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe   (~13 MB)
```

### Build Info (current)

| Component | Version |
|-----------|---------|
| Scintilla | 5.6.4 |
| Lexilla | 5.5.1 |
| Boost Regex | 1_90 |

## Build: Visual Studio (requires C++ workload)

```bash
MSYS_NO_PATHCONV=1 "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
  PowerEditor/visual.net/notepadPlus.sln /m /p:configuration=Release /p:platform=x64
```

> ⚠️ `MSYS_NO_PATHCONV=1` is required in Git Bash / MSYS2 to prevent `/p:` from being converted to a path.
