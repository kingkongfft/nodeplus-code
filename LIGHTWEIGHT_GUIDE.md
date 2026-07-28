# Notepad++ Lightweight Optimization Guide

## Size Breakdown (13 MB release build)

| Component | Size | Pct | Description |
|-----------|------|-----|-------------|
| `liblexilla.a` | 3.9 MB | 30% | 120+ language syntax highlighters |
| `libscintilla.a` | 3.7 MB | 28% | Scintilla editor engine |
| Resources (.res) | ~2.5 MB | 19% | Icons, dialogs, manifest |
| Notepad++ own code | ~3.0 MB | 23% | All .o files |

---

## Method 1: UPX Compression ⭐ Recommended

**Easiest, biggest immediate win. No rebuild needed.**

```bash
# Install UPX
export PATH="/c/msys64/usr/bin:$PATH"
pacman -S mingw-w64-x86_64-upx

# Compress
upx -9 -o notepad++.min.exe notepad++.exe
```

**Result: 13 MB → 6.2 MB (-51%)**

Startup time unaffected (in-place decompression). Functionally identical.

---

## Method 2: `-Os` Optimization

Replace `-O3` with `-Os` in `PowerEditor/gcc/makefile` line 72:

```makefile
# Before:
CXXFLAGS += -O3

# After:
CXXFLAGS += -Os
```

Or pass on command line (needs makefile support):
```bash
mingw32-make -j$(nproc) CXXFLAGS_EXTRA="-Os"
```

**Expected: additional ~10-15% size reduction.**

Combine with UPX for ~4-5 MB total.

---

## Method 3: Trim Lexers ✂️

The biggest code bloat. Edit `lexilla/src/Lexilla.cxx` and comment out unused language linkers:

```cpp
// Example: keep only 15 languages
// LINK_LEXER("LexAda.cxx");
// LINK_LEXER("LexFortran.cxx");
// LINK_LEXER("LexCOBOL.cxx");
LINK_LEXER("LexCPP.cxx");       // C/C++
LINK_LEXER("LexPython.cxx");
LINK_LEXER("LexJSON.cxx");
LINK_LEXER("LexHTML.cxx");
LINK_LEXER("LexXML.cxx");       // ~30-80 KB each
// ... etc
```

Each lexer ~30-80 KB. Removing 80 unused lexers saves **2-4 MB** before UPX.

---

## Method 4: Lean Resources

- Reduce icon resolutions in `PowerEditor/src/icons/`
- Strip unused localization files from `localization/`
- Remove unused default themes from `themes/`

---

## Results Summary

| Method | Size After | Reduction | Rebuild? |
|--------|-----------|-----------|----------|
| Original | **13.0 MB** | — | — |
| + UPX | **6.2 MB** | -51% | No |
| + `-Os` + UPX | **~4.5 MB** | -65% | Yes |
| + Trim lexers + `-Os` + UPX | **~2.5 MB** | -81% | Yes |
