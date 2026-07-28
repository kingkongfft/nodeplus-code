# Terminal Bug Fix: `ls` / `ls -al` Output Garbled / Blank Lines

**Date**: 2026-07-28
**File**: `PowerEditor/src/WinControls/TerminalPanel/TerminalPanel.cpp`
**Reported symptom**: Running `ls` (or `ls -al`, `Get-ChildItem` alias in PowerShell) in the
embedded terminal produced misaligned / overlapping text, and later, entries that were blank
where a directory or colorized filename should have appeared.

This was actually **two separate bugs**, found and fixed in sequence while investigating the
same user report.

---

## Bug A: Prompt redraw overwrites earlier scrollback (`_screenTop` never advances)

### Root cause

`newLine()` (called for every `\n` in the output, i.e. once per line of `ls` output) only
incremented `_cursorRow`. It never advanced `_screenTop` — the baseline used to convert
*absolute* cursor-positioning escape sequences (CUP `\e[row;colH`, VPA `\e[d`) into buffer row
indices:

```cpp
// handleANSIEscape(), case 'H'/'f':
_cursorRow = _screenTop + static_cast<size_t>(row);
```

On a real terminal, once the cursor drops below the bottom of the visible screen the screen
"scrolls" — the viewport's top line advances. Our `_screenTop` never did this on ordinary
line-feeds (it only ever decreased, when the scrollback buffer was trimmed at `_maxLines`).

Once `ls` printed more lines than fit on screen, PSReadLine's prompt-redraw logic issued CUP
sequences relative to "the current screen". We resolved those against a stale, too-small
`_screenTop`, so the redraw landed on top of **earlier `ls` output** instead of the actual
bottom line — producing overlapping / misaligned text.

### Fix (`TerminalPanel.cpp`, `newLine()`)

```cpp
void TerminalPanel::newLine()
{
	_cursorRow++;
	ensureRow(_cursorRow);

	// Once the cursor drops below the bottom of the visible screen, a real
	// terminal scrolls the screen up by one line. _screenTop must advance to
	// match — it is the baseline that absolute cursor positioning (CUP '[H'
	// and VPA '[d') is measured from.
	const size_t rows = static_cast<size_t>(_rows > 0 ? _rows : 24);
	if (_cursorRow >= _screenTop + rows)
		_screenTop = _cursorRow - rows + 1;

	while (_buffer.size() > _maxLines)
	{
		...
	}
}
```

This fixed the overlapping-text case, but a second, unrelated defect was still visible: some
directory/colorized entries rendered as **blank lines** instead of overlapping text.

---

## Bug B: Bare `\e[K` / `\e[J` treated as "erase backwards" instead of "erase forwards"

### Root cause

Captured via raw PTY byte logging (`NPP_TERMINAL_DEBUG=1`, `%TEMP%\npp_terminal_debug.log`).
PowerShell emits directory/colorized entries like this:

```
drwxrwxrwx 1 WaterZhong  WaterZhong  0 Jun 1 10:27  \e[34m\e[1mtest1\e[m\e[K\r\n
                                                     ^^^^^^^^^^^^^^^^^^^ ^^^^ ^^^^
                                                     blue+bold  "test1"  reset  EL (no param)
```

`\e[K` is **EL — Erase in Line**. Per ECMA-48 / xterm, when the parameter is omitted the
default is **0** ("erase from cursor to end of line"). Likewise, bare `\e[J` (**ED — Erase in
Display**) defaults to **0** ("erase from cursor to end of screen").

`handleANSIEscape()` had a single blanket default for *all* omitted-parameter commands:

```cpp
if (params.empty()) params.push_back(cmd == 'm' ? 0 : 1);   // <-- bug: J/K should be 0, not 1
```

`1` is the correct default for cursor-*movement* commands (`A`/`B`/`C`/`D`/etc. — "move by one
cell"), but for the two *erase* commands `J`/`K` the omitted-parameter default must also be 0,
same as SGR. Because it defaulted to `1`, the `K` handler took the wrong branch:

```cpp
// case 'K': (before fix, param resolved to 1 instead of 0)
int start  = (params[0] == 1) ? 0 : ...;                 // -> start = 0 (line start)
int endCol = (params[0] == 1) ? (_cursorCol + 1) : ...;  // -> endCol = cursor+1
```

i.e. it erased **from the start of the line to the cursor** instead of **from the cursor to the
end of the line**. Since the cursor was sitting right after the just-printed filename, this
wiped out the entire line that had just been written — turning colorized directory/symlink
entries into blank lines in the visible output.

### Fix (`TerminalPanel.cpp`, `handleANSIEscape()`)

```cpp
// Default parameter when omitted differs by command:
//   - SGR ('m') and the erase commands ED ('J') / EL ('K') default to 0.
//   - Cursor-movement commands (A/B/C/D/etc.) default to 1 (move by one).
if (params.empty())
	params.push_back((cmd == 'm' || cmd == 'J' || cmd == 'K') ? 0 : 1);
```

`J` (ED) already had a correct branch for `params[0] == 0` ("erase from cursor to end of
screen"), so once the default resolves to `0` it now falls into the right branch automatically
— no other change needed there.

---

## Diagnosis method (for future terminal bugs)

1. Temporarily enabled raw-byte logging inside `processOutput()` (guarded by the existing
   `NPP_TERMINAL_DEBUG` env var / `termLogEnabled()`), dumping every chunk read from the PTY
   with control bytes escaped (`\e`, `\r`, `\n`, `\xNN`) before any parsing.
2. Rebuilt, launched with `NPP_TERMINAL_DEBUG=1`, reproduced `ls -al` in the panel.
3. Killed the process (log file is opened exclusively) and read
   `%TEMP%\npp_terminal_debug.log` — this showed the exact escape sequences PowerShell/PSReadLine
   emitted around each garbled entry, which pointed straight at the `\e[K` default-parameter bug.
4. The raw-byte dump added to `processOutput()` is temporary/diagnostic and gated behind the
   existing debug flag; it can be left in place (zero cost when `NPP_TERMINAL_DEBUG` is unset)
   or removed once confidence is high enough that this class of bug won't recur.

## Verification

- Rebuilt via MinGW-w64 (`cd PowerEditor/gcc && mingw32-make -j8 PREBUILD_EVENT_CMD=":"`) —
  clean build, no new warnings related to this change.
- Manually reproduced in-app: opened PowerShell terminal panel, ran `ls -al` in a directory with
  many colorized (directory/symlink) entries spanning more than one screenful.
- Before fix: colorized entries rendered as blank lines; long output also showed
  overlapping/misaligned text from the stale-`_screenTop` bug.
- After fix: all entries (files, directories, symlinks) render with their names intact, no
  overlapping text, confirmed by user screenshot comparison.
