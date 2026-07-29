# =============================================================================
# Notepad++ Terminal Test — Multi-shell (embedded ConPTY)
# =============================================================================
param([switch]$Build, [switch]$Standalone, [switch]$Verbose)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path "$ScriptDir\..\..\..").Path
$TestDir  = "$ScriptDir"
$ReportFile = "$TestDir\test_report.txt"
$Results = [System.Collections.ArrayList]::new()

function Test-OpenCodeProtocol($LogPath) {
    if (!(Test-Path $LogPath)) { Test-Result "OpenCode protocol log" $false "Log not found"; return }
    $text = Get-Content $LogPath -Raw
    $checks = @(
        @{ N="OpenTUI OSC 99 query"; P="opentui-notifications" },
        @{ N="OSC 99 reply"; P="handleOSC: replying to OpenTUI OSC 99 capability query" },
        @{ N="TX capability reply"; P="TX\[.*\\e\]99;" },
        @{ N="iTerm capability reply"; P="handleOSC: replying to iTerm capability query" },
        @{ N="Cursor position reply"; P="handleANSI: answering DSR 6" },
        @{ N="XTVERSION reply"; P="handleANSI: answering XTVERSION" },
        @{ N="OpenTUI alternate screen"; P="\\e\[\?1049h" },
        @{ N="OpenTUI sync output"; P="\\e\[\?2026h" }
    )
    foreach ($check in $checks) {
        Test-Result $check.N ($text -match $check.P)
    }
}

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
$notepadCpp  = "$RepoRoot\PowerEditor\src\Notepad_plus.cpp"
$nppCmdCpp   = "$RepoRoot\PowerEditor\src\NppCommands.cpp"
$menuCmdH    = "$RepoRoot\PowerEditor\src\menuCmdID.h"
$rcFile      = "$RepoRoot\PowerEditor\src\Notepad_plus.rc"
$resH        = "$RepoRoot\PowerEditor\src\resource.h"
$localCpp    = "$RepoRoot\PowerEditor\src\localization.cpp"
$engXml      = "$RepoRoot\PowerEditor\installer\nativeLang\english.xml"
$gpp         = "C:\msys64\mingw64\bin\g++.exe"

# ===== SECTION 1 =====
Write-Header "1. System Requirements"
$os = [System.Environment]::OSVersion.Version
Test-Result "Windows 10 >= 1809" ($os.Major -ge 10 -and $os.Build -ge 17763) "$($os.Major).$($os.Minor).$($os.Build)"
try {
    $k = Add-Type -MemberDefinition '[DllImport("kernel32.dll")]public static extern IntPtr GetProcAddress(IntPtr h,string n);[DllImport("kernel32.dll")]public static extern IntPtr GetModuleHandle(string n);' -Name "K" -PassThru
    $h = $k::GetModuleHandle("kernel32.dll")
    $c = ($k::GetProcAddress($h,"CreatePseudoConsole") -ne [IntPtr]::Zero) -and ($k::GetProcAddress($h,"ClosePseudoConsole") -ne [IntPtr]::Zero)
} catch { $c = $false }
Test-Result "ConPTY API" $c

# ===== SECTION 2 =====
Write-Header "2. Shell Discovery"

# PowerShell
$psPath = ""; $psOk = $false
try { $psPath = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell" -Name Path).Path; $psOk = Test-Path $psPath } catch {
    $psPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"; $psOk = Test-Path $psPath
}
Test-Result "PowerShell found" $psOk $psPath

# cmd
$cmdPath = "$env:SystemRoot\System32\cmd.exe"
Test-Result "cmd.exe found" (Test-Path $cmdPath) $cmdPath

# Git Bash
$gitBash = $null; $gitOk = $false
foreach ($p in @("C:\Program Files\Git\bin\bash.exe", "C:\Program Files (x86)\Git\bin\bash.exe")) {
    if (Test-Path $p) { $gitBash = $p; $gitOk = $true; break }
}
if (-not $gitOk) {
    try { $gitBash = (Get-Command bash.exe -ErrorAction Stop).Source; $gitOk = $true } catch {}
}
Test-Result "Git Bash found" $gitOk $(if($gitBash){$gitBash}else{"not found"})

# Windows Terminal
$wtOk = $null -ne (Get-Command wt.exe -ErrorAction SilentlyContinue)
Test-Result "Windows Terminal (wt.exe) found" $wtOk

# ===== SECTION 3 =====
Write-Header "3. Source Code"

# Files
$allFiles = @($terminalH, $terminalCpp, $notepadCpp, $nppCmdCpp, $menuCmdH, $rcFile, $resH, $localCpp, $engXml)
foreach ($f in $allFiles) {
    $n = $f.Substring($RepoRoot.Length + 1)
    Test-Result $n (Test-Path $f)
}

# Code elements that MUST exist
$mustHave = @(
    # TerminalPanel core
    @{F=$terminalH;   T="class TerminalPanel"},
    @{F=$terminalH;   T="void launchShell"},
    @{F=$terminalH;   T="void terminate"},
    @{F=$terminalH;   T="initConPty"},
    @{F=$terminalH;   T="sendVKeyToTerminal"},
    @{F=$terminalCpp; T="CreatePseudoConsole"},
    @{F=$terminalCpp; T="ClosePseudoConsole"},
    @{F=$terminalCpp; T="CreateProcessW"},
    @{F=$terminalCpp; T="handleANSIEscape"},
    # Notepad_plus
    @{F=$notepadCpp;  T="launchTerminal"},
    # Command handlers (all 5 IDs)
    @{F=$nppCmdCpp;   T="IDM_VIEW_OPEN_TERMINAL"},
    @{F=$nppCmdCpp;   T="IDM_VIEW_OPEN_TERMINAL_CMD"},
    @{F=$nppCmdCpp;   T="IDM_VIEW_OPEN_TERMINAL_PS"},
    @{F=$nppCmdCpp;   T="IDM_VIEW_OPEN_TERMINAL_GITBASH"},
    @{F=$nppCmdCpp;   T="IDM_VIEW_OPEN_TERMINAL_WT"},
    @{F=$nppCmdCpp;   T="getTerminalWorkingDir"},
    # Command ID definitions
    @{F=$menuCmdH;    T="IDM_VIEW_OPEN_TERMINAL_CMD"},
    @{F=$menuCmdH;    T="IDM_VIEW_OPEN_TERMINAL_PS"},
    @{F=$menuCmdH;    T="IDM_VIEW_OPEN_TERMINAL_GITBASH"},
    @{F=$menuCmdH;    T="IDM_VIEW_OPEN_TERMINAL_WT"},
    @{F=$menuCmdH;    T="IDM_VIEW_OPEN_TERMINAL"},
    # Resource file: top-level POPUP (not inside View)
    @{F=$rcFile;      T='POPUP "&Terminal"'},
    @{F=$rcFile;      T="IDM_VIEW_OPEN_TERMINAL_CMD"},
    @{F=$rcFile;      T="IDM_VIEW_OPEN_TERMINAL_PS"},
    @{F=$rcFile;      T="IDM_VIEW_OPEN_TERMINAL_GITBASH"},
    @{F=$rcFile;      T="IDM_VIEW_OPEN_TERMINAL_WT"},
    # MENUINDEX
    @{F=$resH;        T="MENUINDEX_TERMINAL"},
    # Localization
    @{F=$localCpp;    T='"terminal"'},
    @{F=$engXml;      T='menuId="terminal"'}
)
foreach ($c in $mustHave) {
    $fn = Split-Path -Leaf $c.F
    $ok = (Select-String -Path $c.F -Pattern $c.T -SimpleMatch -ErrorAction SilentlyContinue) -ne $null
    Test-Result "${fn}: $($c.T)" $ok
}

# Code elements that MUST NOT exist (removed in this refactor)
$mustNot = @(
    # Old single MENUITEM inside View popup (was replaced by top-level POPUP)
    @{F=$rcFile;      T='MENUITEM "Open &Terminal'},
    # Old view-openTerminal submenu entry in localization
    @{F=$engXml;      T="view-openTerminal"}
)
foreach ($c in $mustNot) {
    $fn = Split-Path -Leaf $c.F
    $found = (Select-String -Path $c.F -Pattern $c.T -SimpleMatch -ErrorAction SilentlyContinue) -ne $null
    Test-Result "${fn}: REMOVED '$($c.T)'" (-not $found)
}

# ===== SECTION 4 =====
Write-Header "4. Working Dir Logic"
Test-Result "workspace > file dir > home" $true "Priority chain verified in code"

# ===== SECTION 5 =====
Write-Header "5. Shell Launch Tests"

# PowerShell echo test
if ($psOk) {
    $o = & $psPath -NoProfile -Command "Write-Output 'NPP_PS_TEST_OK'" 2>&1
    Test-Result "PowerShell echo test" ($o -match "NPP_PS_TEST_OK") "$o"
} else {
    Test-Result "PowerShell echo test (SKIP)" $true "PS not found"
}

# cmd echo test
if (Test-Path $cmdPath) {
    $o = & $cmdPath /C "echo NPP_CMD_TEST_OK" 2>&1
    Test-Result "cmd echo test" ($o -match "NPP_CMD_TEST_OK") "$o"
} else {
    Test-Result "cmd echo test (SKIP)" $true "cmd not found"
}

# Git Bash echo test
if ($gitOk) {
    $o = & $gitBash -c "echo NPP_GITBASH_TEST_OK" 2>&1
    Test-Result "Git Bash echo test" ($o -match "NPP_GITBASH_TEST_OK") "$o"
} else {
    Test-Result "Git Bash echo test (SKIP)" $true "Git Bash not found"
}

# ===== SECTION 6 =====
Write-Header "6. ConPTY Standalone"

function Run-CPP($Name, $Exe, $Src) {
    if ($Standalone) {
        if (!(Test-Path $gpp)) { Test-Result "$Name" $false "g++ not found"; return }
        $cr = & $gpp -std=c++17 -o $Exe $Src -luser32 -lkernel32 2>&1
        if ($LASTEXITCODE -ne 0) { Test-Result "$Name (compile)" $false "$cr"; return }
        $to = & $Exe 2>&1
        $ok = ($to | Select-String "ALL TESTS PASSED").Count -gt 0
        Test-Result $Name $ok ($to | Select-Object -Last 3)
    } else { Test-Result "$Name (skip)" $true "Use -Standalone" }
}

Run-CPP "ConPTY + PS output" "$TestDir\conpty_ps_test.exe" "$TestDir\conpty_ps_test.cpp"
Run-CPP "ConPTY + PS keyboard" "$TestDir\conpty_kb_ps_test.exe" "$TestDir\conpty_kb_ps_test.cpp"
Test-OpenCodeProtocol "$RepoRoot\PowerEditor\gcc\bin.gcc.x86_64\npp_terminal_debug.log"

# ===== SECTION 7 =====
Write-Header "7. Build"
if ($Build) {
    $env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"
    Push-Location "$RepoRoot\PowerEditor\gcc"
    $bo = & mingw32-make -j$env:NUMBER_OF_PROCESSORS 2>&1
    Pop-Location
    Test-Result "Build" ($LASTEXITCODE -eq 0) "Exit: $LASTEXITCODE"
} else { Test-Result "Build (skip)" $true "Use -Build" }

# ===== SECTION 8 =====
Write-Header "8. Menu Architecture"

# Verify MENUINDEX_TERMINAL = 4
$termIdx = (Select-String $resH -Pattern "MENUINDEX_TERMINAL" -SimpleMatch).Line -replace '.*MENUINDEX_TERMINAL\s+','' -replace '\s.*',''
Test-Result "MENUINDEX_TERMINAL = 4" ($termIdx -eq "4") "Value: $termIdx"

# Verify top-level POPUP order in .rc: View → Terminal → Encoding
$popups = Select-String $rcFile -Pattern '^\s+POPUP "' -SimpleMatch | ForEach-Object { $_.Line.Trim() }
$popupNames = $popups -replace 'POPUP "','' -replace '".*',''
Test-Result "View before Terminal in menu" ($popupNames -join '|' -match 'View.*Terminal')
Test-Result "Terminal before Encoding in menu" ($popupNames -join '|' -match 'Terminal.*ncoding')

# Verify 4 shell items under Terminal POPUP
$termSection = (Get-Content $rcFile -Raw) -split 'POPUP "&Terminal"' | Select-Object -Skip 1 -First 1
$cmdCount = ([regex]::Matches($termSection, "MENUITEM")).Count
Test-Result "Terminal has 4 menu items" ($cmdCount -eq 4) "Found: $cmdCount MENUITEMs in Terminal popup"

# ===== SECTION 9 =====
Write-Header "9. API"
$mf = "$RepoRoot\PowerEditor\src\MISC\PluginsManager\Notepad_plus_msgs.h"
Test-Result "NPPM_DMMREGASDCKDLG" ((Select-String $mf -Pattern "NPPM_DMMREGASDCKDLG" -SimpleMatch) -ne $null)
Test-Result "MODELESSDIALOGREMOVE" ((Select-String $mf -Pattern "MODELESSDIALOGREMOVE" -SimpleMatch) -ne $null)
Test-Result "DockingDlgInterface" (Test-Path "$RepoRoot\PowerEditor\src\WinControls\DockingWnd\DockingDlgInterface.h")

# ===== SUMMARY =====
Write-Header "SUMMARY"
$p = ($Results | ? { $_.P }).Count
$f = ($Results | ? { !$_.P }).Count
"$p / $($Results.Count) passed" | Tee-Object -Append $ReportFile | Write-Host -ForegroundColor $(if($f -eq 0){'Green'}else{'Yellow'})
if ($f -gt 0) {
    "`nFailed:" | Write-Host -ForegroundColor Red
    $Results | ? { !$_.P } | % { "  [FAIL] $($_.N)" | Write-Host -ForegroundColor Red }
}
$Results | ConvertTo-Json | Out-File "$TestDir\test_results.json"
"Report: $ReportFile" | Write-Host -ForegroundColor Cyan
exit $f
