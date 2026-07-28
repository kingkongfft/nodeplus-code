# Plan: Add VS Code-style Terminal Submenu to Notepad++

## Goal

Add a new `View → Open Terminal` submenu with:
- **cmd** (Command Prompt)
- **PowerShell**
- **Git Bash** (auto-detect MSYS2 / Git for Windows)
- **Windows Terminal** (wt.exe)

Plus persist the user's **default terminal preference** in `config.xml`.

## Implementation Plan

### Step 1 — `PowerEditor/src/menuCmdID.h`: Add command IDs

After `IDM_VIEW_NPC_CCUNIEOL` (`IDM_VIEW + 131`), use the `IDM_VIEW + 200` range:

```cpp
// Terminal submenu (IDM_VIEW + 200)
#define    IDM_VIEW_OPEN_TERMINAL              (IDM_VIEW + 200)
    #define    IDM_VIEW_OPEN_TERMINAL_CMD       (IDM_VIEW_OPEN_TERMINAL + 1)   // 44201
    #define    IDM_VIEW_OPEN_TERMINAL_PS        (IDM_VIEW_OPEN_TERMINAL + 2)   // 44202
    #define    IDM_VIEW_OPEN_TERMINAL_GITBASH   (IDM_VIEW_OPEN_TERMINAL + 3)   // 44203
    #define    IDM_VIEW_OPEN_TERMINAL_WT        (IDM_VIEW_OPEN_TERMINAL + 4)   // 44204
```

**Complexity:** Trivial | **Lines:** +6

---

### Step 2 — `PowerEditor/src/Parameters.h`: Terminal type enum + setting

**a)** Add enum (near line 710, before `NppGUI` struct):

```cpp
enum TerminalType : int { term_cmd = 0, term_powershell = 1, term_gitbash = 2, term_wt = 3 };
```

**b)** Add field to `NppGUI` struct (near `_muteSounds`, ~line 842):

```cpp
TerminalType _defaultTerminal = term_cmd;
```

**Complexity:** Trivial | **Lines:** +4

---

### Step 3 — `PowerEditor/src/Parameters.cpp`: Save/load default terminal

**a) Load** — in the `<GUIConfig name="MISC">` section (after `_hideMenuRightShortcuts`, ~line 6587):

```cpp
_nppGUI._defaultTerminal = static_cast<TerminalType>(
    NppXml::intAttribute(childNode, "defaultTerminal",
        static_cast<int>(_nppGUI._defaultTerminal)));
```

**b) Save** — in the MISC save section (after `hideMenuRightShortcuts`, ~line 7709):

```cpp
NppXml::setAttribute(GUIConfigElement, "defaultTerminal",
    static_cast<int>(_nppGUI._defaultTerminal));
```

**Complexity:** Trivial | **Lines:** +3

---

### Step 4 — `PowerEditor/src/Notepad_plus.rc`: Add submenu to View menu

Insert after `IDM_VIEW_HIDELINES` line (before the `SEPARATOR` + `"Fold All"`):

```rc
        MENUITEM SEPARATOR
        POPUP "Open &Terminal"
        BEGIN
            MENUITEM "&cmd\tAlt+F1",               IDM_VIEW_OPEN_TERMINAL_CMD
            MENUITEM "&PowerShell\tAlt+F2",        IDM_VIEW_OPEN_TERMINAL_PS
            MENUITEM "&Git Bash\tAlt+F3",          IDM_VIEW_OPEN_TERMINAL_GITBASH
            MENUITEM "&Windows Terminal\tAlt+F4",  IDM_VIEW_OPEN_TERMINAL_WT
        END
```

**Complexity:** Easy | **Lines:** +8

---

### Step 5 — `PowerEditor/src/NppCommands.cpp`: Implement terminal launching

**5a) Helper — find Git Bash location:**

```cpp
static std::wstring findGitBash()
{
    // 1. Check MSYS2
    std::wstring path = L"C:\\msys64\\usr\\bin\\bash.exe";
    if (::PathFileExistsW(path.c_str()))
        return path;

    // 2. Check Git for Windows (registry)
    HKEY hKey;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\GitForWindows", 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH]{};
        DWORD sz = sizeof(buf);
        if (::RegGetValueW(hKey, nullptr, L"InstallPath",
            RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        {
            ::RegCloseKey(hKey);
            return std::wstring(buf) + L"\\bin\\bash.exe";
        }
        ::RegCloseKey(hKey);
    }

    // 3. Check 32-bit Git for Windows
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WOW6432Node\\GitForWindows", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH]{};
        DWORD sz = sizeof(buf);
        if (::RegGetValueW(hKey, nullptr, L"InstallPath",
            RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        {
            ::RegCloseKey(hKey);
            return std::wstring(buf) + L"\\bin\\bash.exe";
        }
        ::RegCloseKey(hKey);
    }

    // 4. Check common install path
    path = L"C:\\Program Files\\Git\\bin\\bash.exe";
    if (::PathFileExistsW(path.c_str()))
        return path;

    return L"";
}
```

**5b) Command cases** (after `IDM_FILE_OPEN_POWERSHELL` case, ~line 269):

```cpp
case IDM_VIEW_OPEN_TERMINAL_CMD:
{
    Command cmd(L"cmd.exe");
    cmd.run(_pPublicInterface->getHSelf(), L"$(CURRENT_DIRECTORY)");
}
break;

case IDM_VIEW_OPEN_TERMINAL_PS:
{
    // Reuse existing PowerShell path lookup logic
    static wchar_t psPath[512] = {L'\0'};
    if (psPath[0] == L'\0')
    {
        const wchar_t* subkey = L"SOFTWARE\\Microsoft\\PowerShell\\1\\ShellIds\\Microsoft.PowerShell";
        const wchar_t* valueName = L"Path";
        HKEY hKey = nullptr;
        LONG status = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &hKey);
        if (status != ERROR_SUCCESS) break;
        DWORD bufSize = sizeof(psPath);
        status = ::RegGetValueW(hKey, nullptr, valueName, RRF_RT_REG_SZ, nullptr, psPath, &bufSize);
        ::RegCloseKey(hKey);
        if (status != ERROR_SUCCESS) break;
    }
    Command ps(psPath);
    ps.run(_pPublicInterface->getHSelf(), L"$(CURRENT_DIRECTORY)");
}
break;

case IDM_VIEW_OPEN_TERMINAL_GITBASH:
{
    std::wstring gitBash = findGitBash();
    if (gitBash.empty())
    {
        ::MessageBoxW(_pPublicInterface->getHSelf(),
            L"Git Bash not found. Install MSYS2 or Git for Windows.",
            L"Open Terminal", MB_OK | MB_ICONWARNING);
        break;
    }
    Command bash(gitBash);
    bash.run(_pPublicInterface->getHSelf(), L"$(CURRENT_DIRECTORY)");
}
break;

case IDM_VIEW_OPEN_TERMINAL_WT:
{
    Command wt(L"wt.exe");
    wt.run(_pPublicInterface->getHSelf(), L"$(CURRENT_DIRECTORY)");
}
break;
```

**Complexity:** Medium | **Lines:** +65

---

### Step 6 — `PowerEditor/installer/nativeLang/english.xml`: Localization

Add these entries in the appropriate sections of the XML:

```xml
<!-- Sub Menu Entries (in <SubEntries>) -->
<Item subMenuId="view-openTerminal" name="Open &amp;Terminal"/>

<!-- Submenu items (in <SubEntries> for id-based items) -->
<Item id="44201" name="cmd"/>
<Item id="44202" name="PowerShell"/>
<Item id="44203" name="Git Bash"/>
<Item id="44204" name="Windows Terminal"/>

<!-- Tab context menu / Run dialog (in <Commands>) -->
<Item CMDID="44201" name="Open Terminal (cmd)"/>
<Item CMDID="44202" name="Open Terminal (PowerShell)"/>
<Item CMDID="44203" name="Open Terminal (Git Bash)"/>
<Item CMDID="44204" name="Open Terminal (Windows Terminal)"/>
```

**Complexity:** Easy | **Lines:** +10

---

### Step 7 (Optional) — `Preference` dialog: Default terminal picker

Add a `COMBOBOX` to `preference.rc` MISC page:

```rc
COMBOBOX  IDC_COMBO_DEFAULT_TERMINAL, X, Y, W, H, CBS_DROPDOWNLIST | WS_TABSTOP
```

And wire it in `preferenceDlg.cpp` with options: cmd / PowerShell / Git Bash / Windows Terminal.

**Complexity:** Medium | **Lines:** ~30

---

## Summary

| Step | File | Lines | Complexity |
|------|------|-------|------------|
| 1 | `menuCmdID.h` | +6 | Trivial |
| 2 | `Parameters.h` | +4 | Trivial |
| 3 | `Parameters.cpp` | +3 | Trivial |
| 4 | `Notepad_plus.rc` | +8 | Easy |
| 5 | `NppCommands.cpp` | +65 | Medium |
| 6 | `english.xml` | +10 | Easy |
| 7* | `preferenceDlg.rc` + `.cpp` | ~30 | Medium |
| **Total (steps 1-6)** | | **~96 lines** | |

> *Step 7 is optional. Without it, all four terminals are accessible via the `View > Open Terminal` submenu and can be bound to keyboard shortcuts, but no "default terminal" is configurable in Preferences.

## Generated IDs Reference

| Constant | Value | Purpose |
|----------|-------|---------|
| `IDM_VIEW_OPEN_TERMINAL` | 44200 | Submenu anchor |
| `IDM_VIEW_OPEN_TERMINAL_CMD` | 44201 | Launch cmd.exe |
| `IDM_VIEW_OPEN_TERMINAL_PS` | 44202 | Launch PowerShell |
| `IDM_VIEW_OPEN_TERMINAL_GITBASH` | 44203 | Launch Git Bash |
| `IDM_VIEW_OPEN_TERMINAL_WT` | 44204 | Launch Windows Terminal |

## Detection Priority for Git Bash

1. `C:\msys64\usr\bin\bash.exe` (MSYS2)
2. `HKLM\SOFTWARE\GitForWindows\InstallPath\bin\bash.exe` (Git for Windows 64-bit)
3. `HKLM\SOFTWARE\WOW6432Node\GitForWindows\InstallPath\bin\bash.exe` (Git for Windows 32-bit)
4. `C:\Program Files\Git\bin\bash.exe` (default path)
