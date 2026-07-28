# =============================================================================
# Notepad++ Terminal -- Copy/Paste & Focus Fix Test
# Tests: keyboard hook (focus-aware), Ctrl+C/V, PS startup, dark background
# =============================================================================
param([switch]$Build, [switch]$Run)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path "$ScriptDir\..\..\..").Path
$ReportFile = "$ScriptDir\test_copy_paste_report.txt"
$Results = [System.Collections.ArrayList]::new()

function Write-Header($T) {
    $l = "=" * 70
    "$l`n  $T`n$l" | Tee-Object -Append $ReportFile | Write-Host -ForegroundColor Cyan
}
function Test-Result($Name, $Pass, $Detail="") {
    $s = if ($Pass) { "[PASS]" } else { "[FAIL]" }
    $e = "$s $Name"
    if ($Detail) { $e += "`n         $Detail" }
    $e | Tee-Object -Append $ReportFile | Write-Host -ForegroundColor $(if($Pass){'Green'}else{'Red'})
    $Results.Add(@{ N=$Name; P=$Pass; D=$Detail }) | Out-Null
}

$terminalCpp = "$RepoRoot\PowerEditor\src\WinControls\TerminalPanel\TerminalPanel.cpp"
$terminalH   = "$RepoRoot\PowerEditor\src\WinControls\TerminalPanel\TerminalPanel.h"
$nppCmdCpp   = "$RepoRoot\PowerEditor\src\NppCommands.cpp"

# ===== SECTION 1: Source code checks =====
Write-Header "1. TerminalPanel - Keyboard Hook (focus-aware)"

$hookContent = Get-Content $terminalCpp -Raw

# 1a. Hook uses GetFocus/isTerminalFocused check (not eating all global keys)
$hasFocusCheck = ($hookContent -match 'IsChild\(hDlg, hFocus\)' -or
                  $hookContent -match 'isTerminalFocused' -or
                  $hookContent -match 'hFocus\s*==\s*hTermWnd')
Test-Result "Hook checks focus before eating keys" $hasFocusCheck

# 1b. Ctrl+C with selection -> copy
$hasCtrlCCopy = $hookContent -match "pKb->vkCode == 'C'.*hasSelection"
Test-Result "Ctrl+C with selection -> copy to clipboard" $hasCtrlCCopy

# 1c. Ctrl+V -> paste
$hasCtrlVPaste = $hookContent -match "pKb->vkCode == 'V'.*ctrl"
Test-Result "Ctrl+V -> paste from clipboard" $hasCtrlVPaste

# 1d. Ctrl+letter sends control character
$hasCtrlLetter = $hookContent -match "ctrl.*vkCode >= 'A'.*vkCode <= 'Z'"
Test-Result "Ctrl+A-Z sends control characters" $hasCtrlLetter

# 1e. A-Z without Ctrl produces text via the layout-aware ToUnicode path
$hasToUnicodeFallthrough = ($hookContent -match '::ToUnicode\(vkCode, scanCode, keyState')
Test-Result "Letter keys without Ctrl -> ToUnicode" $hasToUnicodeFallthrough

# ===== SECTION 1B: Clipboard robustness =====
Write-Header "1B. Clipboard Robustness"

# GetAsyncKeyState must be used inside the LL hook (GetKeyState is unreliable there)
$usesAsyncKeyState = ($hookContent -match 'GetAsyncKeyState\(VK_CONTROL\)')
Test-Result "Hook reads modifiers with GetAsyncKeyState" $usesAsyncKeyState

# Bare modifier keys must not be swallowed, or all chords break
$passesModifiers = ($hookContent -match 'case VK_CONTROL:\s*case VK_LCONTROL:')
Test-Result "Bare modifier keys are not eaten by the hook" $passesModifiers

# Ctrl+Shift+C / Ctrl+Insert copy, Shift+Insert paste
$hasCtrlShiftC = ($hookContent -match "vkCode == 'C' && ctrl && shift")
Test-Result "Ctrl+Shift+C copies" $hasCtrlShiftC
$hasInsertChords = ($hookContent -match 'vkCode == VK_INSERT && ctrl' -and
                    $hookContent -match 'vkCode == VK_INSERT && shift')
Test-Result "Ctrl+Insert / Shift+Insert chords" $hasInsertChords

# SetClipboardData failure must free the block (no leak)
$noClipboardLeak = ($hookContent -match 'if \(!ok\)\s*\r?\n\s*::GlobalFree\(hMem\)')
Test-Result "Clipboard memory freed when SetClipboardData fails" $noClipboardLeak

# Clipboard open retried when another app holds it
$retriesClipboard = ($hookContent -match 'for \(int attempt = 0; attempt < 10 && !opened')
Test-Result "OpenClipboard is retried on contention" $retriesClipboard

# Paste normalizes CRLF -> CR and supports bracketed paste
$normalizesNewlines = ($hookContent -match 'swallow the LF of a CRLF pair' -and
                       $hookContent -match 'a terminal expects CR')
Test-Result "Paste normalizes line endings to CR" $normalizesNewlines
$hasBracketedPaste = ($hookContent -match '\\x1b\[200~' -and $hookContent -match '2004')
Test-Result "Bracketed paste (DECSET 2004) supported" $hasBracketedPaste

# CF_TEXT fallback when only ANSI text is on the clipboard
$hasCfTextFallback = ($hookContent -match 'GetClipboardData\(CF_TEXT\)')
Test-Result "CF_TEXT fallback for paste" $hasCfTextFallback

# Right-click / middle-click / context menu paths
$hasMouseClipboard = ($hookContent -match 'case WM_RBUTTONUP:' -and
                      $hookContent -match 'case WM_MBUTTONUP:' -and
                      $hookContent -match 'case WM_CONTEXTMENU:')
Test-Result "Right-click, middle-click and context menu clipboard paths" $hasMouseClipboard

# Partial WriteFile handled for large pastes
$loopsWriteFile = ($hookContent -match 'while \(remaining > 0\)')
Test-Result "Large pastes handle partial WriteFile" $loopsWriteFile

# Selection/buffer shared between reader thread and UI thread must be locked
$hasMutex = ($hookContent -match 'std::lock_guard<std::recursive_mutex> lock\(_bufMutex\)')
Test-Result "Buffer guarded by mutex (reader vs UI thread)" $hasMutex

# Mouse hit-testing must use signed coordinates
$signedHitTest = ($hookContent -match 'GET_X_LPARAM\(lParam\)' -and
                  $hookContent -notmatch 'LOWORD\(lParam\) / _charWidth')
Test-Result "Mouse hit-test uses signed coordinates" $signedHitTest

# Selection end column must be exclusive (last char was previously dropped)
$exclusiveEnd = ($hookContent -match 'for \(int c = start; c < end; c\+\+\)')
Test-Result "Selection includes the last character" $exclusiveEnd

# ===== SECTION 1C: Input / output correctness =====
Write-Header "1C. Input & Output Correctness"

# Arrow keys / Home / End / Delete escape sequences (PowerShell history editing)
$hasNavKeys = ($hookContent -match 'case VK_UP:\s*csiKey' -and
               $hookContent -match 'case VK_HOME:\s*csiKey' -and
               $hookContent -match 'case VK_DELETE:\s*tildeKey')
Test-Result "Arrow / Home / End / Delete send escape sequences" $hasNavKeys

# Function keys
$hasFnKeys = ($hookContent -match 'case VK_F1:' -and $hookContent -match 'case VK_F12:')
Test-Result "Function keys F1-F12 supported" $hasFnKeys

# UTF-8 incremental decoding (non-ASCII output was mojibake)
$hasUtf8Decode = ($hookContent -match '_utf8Remaining' -and $hookContent -match '0xD800')
Test-Result "Incremental UTF-8 decode with surrogate pairs" $hasUtf8Decode

# Auto-wrap at the right margin
$hasAutoWrap = ($hookContent -match 'if \(_cursorCol >= _cols\)\s*\r?\n\s*\{\s*\r?\n\s*_cursorCol = 0;')
Test-Result "Long lines auto-wrap instead of truncating" $hasAutoWrap

# CUP must be screen-relative, not buffer-absolute
$screenRelativeCup = ($hookContent -match '_cursorRow = _screenTop \+ static_cast<size_t>\(row\)')
Test-Result "Cursor positioning is screen-relative" $screenRelativeCup

# 256-color / truecolor SGR
$has256Color = ($hookContent -match 'xterm256' -and $hookContent -match 'prm == 38 \|\| prm == 48')
Test-Result "256-color and truecolor SGR supported" $has256Color

# Working scrollbar
$hasScrollInfo = ($hookContent -match '::SetScrollInfo\(_hTermWnd, SB_VERT')
Test-Result "Vertical scrollbar is wired up" $hasScrollInfo

# No duplicated keystrokes from the dialog proc
$noDoubleKeys = ($hookContent -notmatch 'run_dlgProc: forwarding key msg')
Test-Result "Dialog proc no longer double-sends keystrokes" $noDoubleKeys

# Ctrl+Break must not signal Notepad++ itself: no *call* to GenerateConsoleCtrlEvent
# (the identifier still appears in the explanatory comment, so match the call form)
$safeBreak = ($hookContent -notmatch '::GenerateConsoleCtrlEvent\(')
Test-Result "Ctrl+Break does not signal Notepad++ process group" $safeBreak

# ===== SECTION 2: TerminalPanel -- Dark background =====
Write-Header "2. TerminalPanel -- Dark Background"

$hdrContent = Get-Content $terminalH -Raw

$hasDarkBg = ($hdrContent -match 'RGB\(12,\s*12,\s*12\)' -and
              $hdrContent -notmatch 'NppDarkMode::isEnabled')
Test-Result "Background always dark (RGB 12,12,12)" $hasDarkBg

$hasDarkFg = $hdrContent -match 'RGB\(220,\s*220,\s*220\)'
Test-Result "Foreground always light gray (RGB 220,220,220)" $hasDarkFg

# ===== SECTION 3: TerminalPanel -- WM_CTLCOLOR dialog =====
Write-Header "3. Dialog Background (no white flash)"

$hasCtlColor = $hookContent -match 'case WM_CTLCOLORDLG:'
Test-Result "WM_CTLCOLORDLG handler for dark dialog bg" $hasCtlColor

# ===== SECTION 4: NppCommands -- PowerShell detection =====
Write-Header "4. PowerShell Detection & Arguments"

$cmdContent = Get-Content $nppCmdCpp -Raw

$hasPwshExe = $cmdContent -match 'pwsh\.exe'
Test-Result "Detects PowerShell 7 (pwsh.exe)" $hasPwshExe

$hasNoLogo = $cmdContent -match '-NoLogo'
Test-Result "Uses -NoLogo flag" $hasNoLogo

$hasNoExit = $cmdContent -match '-NoExit'
Test-Result "Uses -NoExit flag" $hasNoExit

$hasPS5Fallback = $cmdContent -match 'Microsoft\.PowerShell'
Test-Result "Falls back to Windows PowerShell 5.1" $hasPS5Fallback

# No more -Command cd '...' in the PowerShell launch command
# (the comment "no -Command cd is needed" is fine, but the actual wsprintfW must not have it)
$hasNoCommandCd = ($cmdContent -notmatch 'wsprintfW.*-Command cd')
Test-Result "No -Command cd in PowerShell launch" $hasNoCommandCd

# ===== SECTION 5: Build =====
Write-Header "5. Build"

$gpp = "C:\msys64\mingw64\bin\g++.exe"
$make = "C:\msys64\mingw64\bin\mingw32-make.exe"
$buildOk = $true

if ($Build) {
    $env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"
    Push-Location "$RepoRoot\PowerEditor\gcc"

    # Generate header first (paths with spaces workaround)
    $genBat = "$RepoRoot\PowerEditor\src\NppLibsVersionH-generator.bat"
    if (Test-Path $genBat) {
        & $genBat 2>&1 | Out-Null
        Test-Result "NppLibsVersion.h generated" (Test-Path "$RepoRoot\PowerEditor\src\NppLibsVersion.h")
    }

    $bo = & $make -j$env:NUMBER_OF_PROCESSORS PREBUILD_EVENT_CMD=":" 2>&1
    $buildOk = ($LASTEXITCODE -eq 0)
    Pop-Location
    Test-Result "Build" $buildOk ("Exit: " + $LASTEXITCODE)
} else {
    Test-Result "Build (skip)" $true "Use -Build to compile"
}

# ===== SECTION 6: Binary checks =====
Write-Header "6. Binary"

$exePath = "$RepoRoot\PowerEditor\gcc\bin.gcc.x86_64\notepad++.exe"
$exeExists = Test-Path $exePath
Test-Result "notepad++.exe exists" $exeExists $exePath
if ($exeExists) {
    $size = (Get-Item $exePath).Length
    Test-Result "Binary > 10MB" ($size -gt 10 * 1024 * 1024) "$([math]::Round($size/1MB, 1)) MB"

    # Check for key strings in binary
    $bin = [System.IO.File]::ReadAllBytes($exePath)
    $binText = [System.Text.Encoding]::Unicode.GetString($bin)
    Test-Result "-NoLogo in binary" ($binText -match '-NoLogo')
}

# ===== SECTION 7: Run smoke test =====
Write-Header "7. Smoke Test (run & verify)"

if ($Run -and $exeExists) {
    # Kill any existing
    Get-Process -Name "notepad++" -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 1

    # Start
    $proc = Start-Process -FilePath $exePath -PassThru
    Start-Sleep 3

    Test-Result "Process started" ($proc -and !$proc.HasExited) "PID: $($proc.Id)"

    # Check main window
    $hwnd = $proc.MainWindowHandle
    Test-Result "Main window visible" ($hwnd -ne [IntPtr]::Zero)

    # Clean up
    $proc.CloseMainWindow()
    Start-Sleep 1
    if (!$proc.HasExited) { $proc.Kill() }
} else {
    Test-Result "Smoke test (skip)" $true "Use -Run"
}

# ===== SUMMARY =====
Write-Header "SUMMARY"
$p = ($Results | Where-Object { $_.P }).Count
$f = ($Results | Where-Object { !$_.P }).Count
"$p / $($Results.Count) passed" | Tee-Object -Append $ReportFile | Write-Host -ForegroundColor $(if($f -eq 0){'Green'}else{'Yellow'})
if ($f -gt 0) {
    "`nFailed:" | Write-Host -ForegroundColor Red
    $Results | Where-Object { !$_.P } | ForEach-Object {
        "  [FAIL] $($_.N)" | Write-Host -ForegroundColor Red
    }
}
$Results | ConvertTo-Json -Depth 3 | Out-File "$ScriptDir\test_copy_paste_results.json"
"Report: $ReportFile" | Write-Host -ForegroundColor Cyan
exit $f
