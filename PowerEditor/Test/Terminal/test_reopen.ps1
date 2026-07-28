# Test: Open → Close → Re-open Terminal (PowerShell)
$exe = "C:\Users\Water_Zhong\OneDrive - EPAM\projects\REPOS-onedrive\notepad-plus-plus\PowerEditor\gcc\bin.gcc.x86_64\notepad++.exe"

# Kill existing
taskkill /f /im notepad++.exe 2>$null | Out-Null
Start-Sleep 1

# Launch
$p = Start-Process -FilePath $exe -PassThru
Start-Sleep 2

# Find window
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32 {
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetSubMenu(IntPtr hMenu, int nPos);
    [DllImport("user32.dll")] public static extern int GetMenuItemID(IntPtr hMenu, int nPos);
    [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr hMenu);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool CloseWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr hWndParent, IntPtr hWndChildAfter, string lpszClass, string lpszWindow);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder lpString, int nMaxCount);
}
"@

$WM_COMMAND = 0x0111
$NPPM_DMMHIDE = 0x2000 + 84  # NPPM_DMMHIDE
$NPPM_DMMSHOW = 0x2000 + 85  # NPPM_DMMSHOW

$hwnd = [Win32]::FindWindow("Notepad++", $null)
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] Could not find Notepad++ window" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Found Notepad++ window: $hwnd" -ForegroundColor Green

# Check menu bar
$hMenu = [Win32]::GetMenu($hwnd)
if ($hMenu -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] No menu bar" -ForegroundColor Red
    exit 1
}

# Find Terminal menu (MENUINDEX_TERMINAL = 4)
$hTermMenu = [Win32]::GetSubMenu($hMenu, 4)
if ($hTermMenu -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] No Terminal menu at position 4" -ForegroundColor Red
    exit 1
}
$count = [Win32]::GetMenuItemCount($hTermMenu)
Write-Host "[OK] Terminal menu at position 4, $count items" -ForegroundColor Green

# Get PowerShell command ID (item 0 in Terminal menu)
$cmdId = [Win32]::GetMenuItemID($hTermMenu, 0)
Write-Host "[OK] Terminal→PowerShell cmd ID: $cmdId"

Write-Host ""
Write-Host "=== Round 1: Open Terminal ===" -ForegroundColor Cyan

# Send command to open PowerShell terminal
[Win32]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$cmdId, [IntPtr]::Zero) | Out-Null
Start-Sleep 2

# Check if terminal window exists (class: NppTerminalWindow)
$termHwnd = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, "NppTerminalWindow", $null)
if ($termHwnd -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] Round 1: Terminal window not found after opening" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Round 1: Terminal window created: $termHwnd" -ForegroundColor Green

Write-Host ""
Write-Host "=== Close Terminal ===" -ForegroundColor Cyan

# Find the docked terminal tab close button or send hide
# Try to find the terminal panel dialog
$termDlg = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, $null, "Terminal")
if ($termDlg -eq [IntPtr]::Zero) {
    # Try to find by class
    $termDlg = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, "#32770", $null)
}

# Send hide message to the terminal panel
# First find the docked window...
# Actually, just send WM_COMMAND to close/hide via the menu toggle
# For now, let's simulate closing by destroying the terminal window
[Win32]::SendMessage($hwnd, $NPPM_DMMHIDE, [IntPtr]::Zero, $termHwnd) | Out-Null
Start-Sleep 1

$termHwnd2 = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, "NppTerminalWindow", $null)
if ($termHwnd2 -ne [IntPtr]::Zero) {
    Write-Host "[WARN] Terminal window still exists after hide" -ForegroundColor Yellow
} else {
    Write-Host "[OK] Terminal window hidden" -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Round 2: Re-open Terminal ===" -ForegroundColor Cyan

[Win32]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$cmdId, [IntPtr]::Zero) | Out-Null
Start-Sleep 2

$termHwnd3 = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, "NppTerminalWindow", $null)
if ($termHwnd3 -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] Round 2: Terminal window NOT found after re-open!" -ForegroundColor Red
    Write-Host "         The bug is still present." -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Round 2: Terminal window re-opened: $termHwnd3" -ForegroundColor Green

Write-Host ""
Write-Host "=== Round 3: Close and Re-open Again ===" -ForegroundColor Cyan

[Win32]::SendMessage($hwnd, $NPPM_DMMHIDE, [IntPtr]::Zero, $termHwnd3) | Out-Null
Start-Sleep 1
[Win32]::PostMessage($hwnd, $WM_COMMAND, [IntPtr]$cmdId, [IntPtr]::Zero) | Out-Null
Start-Sleep 2

$termHwnd4 = [Win32]::FindWindowEx($hwnd, [IntPtr]::Zero, "NppTerminalWindow", $null)
if ($termHwnd4 -eq [IntPtr]::Zero) {
    Write-Host "[FAIL] Round 3: Terminal window NOT found!" -ForegroundColor Red
    exit 1
}
Write-Host "[OK] Round 3: Terminal window re-opened: $termHwnd4" -ForegroundColor Green

Write-Host ""
Write-Host "=== ALL TESTS PASSED ===" -ForegroundColor Green

# Cleanup
Stop-Process -Id $p.Id -Force 2>$null
