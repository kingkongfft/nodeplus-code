// This file is part of Notepad++ project
// Copyright (C)2021 Don HO <don.h@free.fr>

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "TerminalPanel.h"
#include "NppDarkMode.h"
#include "Parameters.h"

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE may not be defined in older SDKs
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <algorithm>
#include <share.h>      // _SH_DENYNO for _wfsopen
#include <strsafe.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM

// Posted by the reader thread so the scrollbar is updated on the UI thread.
#define WM_APP_TERM_SYNCSCROLL (WM_APP + 0x51)
#define WM_APP_TERM_SELECTALL  (WM_APP + 0x52)

// Debounce timer for WM_SIZE -> ResizePseudoConsole. The docking panel fires
// a burst of WM_SIZE while it settles into its final layout (host window
// creation, docking animation, splitter drag). Forwarding every intermediate
// size to ConPTY makes PowerShell/PSReadLine repaint its prompt over and
// over, and it can end up interrupted mid-redraw on a cleared screen with
// nothing drawn yet -- i.e. a terminal that looks permanently blank. Instead
// we buffer the pending size and only push it to the PTY once no further
// WM_SIZE has arrived for RESIZE_DEBOUNCE_MS.
#define IDT_TERM_RESIZE 1001
#define RESIZE_DEBOUNCE_MS 400


// --- Debug logger ---
// Hard-coded on: writes next to notepad++.exe as npp_terminal_debug.log so
// it is trivial to find regardless of how/where the app was launched (no
// env var propagation needed to an already-running GUI process). Flip
// TERM_LOG_HARDCODED_ENABLED to 0 to fall back to the NPP_TERMINAL_DEBUG=1
// opt-in behaviour if the always-on logging becomes a perf concern again.
#define TERM_LOG_HARDCODED_ENABLED 1

static bool termLogEnabled()
{
#if TERM_LOG_HARDCODED_ENABLED
	return true;
#else
	static int enabled = -1;
	if (enabled < 0)
	{
		wchar_t v[8] = {};
		DWORD n = ::GetEnvironmentVariableW(L"NPP_TERMINAL_DEBUG", v, 8);
		enabled = (n > 0 && v[0] != L'0') ? 1 : 0;
	}
	return enabled == 1;
#endif
}

static void termLog(const wchar_t* fmt, ...)
{
	if (!termLogEnabled()) return;

	wchar_t buf[1024];
	va_list args;
	va_start(args, fmt);
	_vsnwprintf(buf, 1024, fmt, args);
	va_end(args);
	buf[1023] = L'\0';
	::OutputDebugStringW(buf);

	// Also write to a file next to the running executable.
	// Use _wfsopen with _SH_DENYNO so external readers (Get-Content, etc.)
	// can open the file concurrently while the process is running.
	static FILE* logFile = nullptr;
	if (!logFile)
	{
		wchar_t logPath[MAX_PATH] = {};
		DWORD n = ::GetModuleFileNameW(nullptr, logPath, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
		{
			// Fallback: TEMP dir if the exe path can't be resolved.
			::GetEnvironmentVariableW(L"TEMP", logPath, MAX_PATH);
			wcscat_s(logPath, L"\\npp_terminal_debug.log");
		}
		else
		{
			wchar_t* lastSlash = wcsrchr(logPath, L'\\');
			if (lastSlash) *(lastSlash + 1) = L'\0';
			else logPath[0] = L'\0';
			wcscat_s(logPath, L"npp_terminal_debug.log");
		}
		logFile = _wfsopen(logPath, L"w", _SH_DENYNO);
	}
	if (logFile)
	{
		fwprintf(logFile, L"%ls\n", buf);
		fflush(logFile);
	}
}
#define TERM_LOG(fmt, ...) termLog(L"[TERM] " fmt, ##__VA_ARGS__)

// --- Keyboard hook for terminal input ---
// WH_KEYBOARD_LL hook captures keys before Scintilla can consume them.
// The hook is installed/removed on focus — active only when the terminal
// window has keyboard focus. When focus moves to the editor, the hook is
// removed so all keystrokes go to the editor normally.
static HHOOK g_termKbHook = nullptr;
static TerminalPanel* g_activeTermPanel = nullptr;

static LRESULT CALLBACK termKbHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && g_activeTermPanel && g_activeTermPanel->isRunning())
	{
		// Only intercept keys when the terminal panel has keyboard focus.
		// IsChild checks the actual HWND parent chain, which is reliable
		// regardless of how the docking manager reparents windows.
		HWND hFocus = ::GetFocus();
		HWND hDlg = g_activeTermPanel->getHSelf();
		HWND hTermWnd = g_activeTermPanel->getTerminalHwnd();
		bool focusedOnTerminal = (hFocus == hTermWnd) || (hFocus == hDlg) ||
			(hDlg && ::IsChild(hDlg, hFocus)) ||
			(hTermWnd && ::IsChild(hTermWnd, hFocus));

		if (!focusedOnTerminal)
			return ::CallNextHookEx(g_termKbHook, nCode, wParam, lParam);
		if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
		{
			KBDLLHOOKSTRUCT* pKb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

			// IMPORTANT: a low-level keyboard hook runs *before* the async key
			// state for the current key is updated in some cases, and GetKeyState
			// only reflects the state of the thread's message queue — which is
			// unreliable inside a WH_KEYBOARD_LL hook. GetAsyncKeyState reads the
			// real hardware state and is the correct API here. Using GetKeyState
			// was the root cause of Ctrl+C / Ctrl+V being missed intermittently.
			const bool ctrl  = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const bool alt   = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
			const bool win   = ((::GetAsyncKeyState(VK_LWIN) | ::GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;

			// Never swallow bare modifier keys — swallowing them prevents Windows
			// from ever updating the modifier state, which breaks every subsequent
			// chord (this is why Ctrl+V often did nothing at all).
			switch (pKb->vkCode)
			{
			case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
			case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
			case VK_MENU:    case VK_LMENU:    case VK_RMENU:
			case VK_LWIN:    case VK_RWIN:
			case VK_CAPITAL: case VK_NUMLOCK:  case VK_SCROLL:
				return ::CallNextHookEx(g_termKbHook, nCode, wParam, lParam);
			default:
				break;
			}

			if (win || (alt && (pKb->vkCode == VK_TAB || pKb->vkCode == VK_F4 ||
			                       pKb->vkCode == VK_SPACE || pKb->vkCode == VK_ESCAPE)))
				return ::CallNextHookEx(g_termKbHook, nCode, wParam, lParam);

			// ---- Clipboard shortcuts ----
			// Copy: Ctrl+Shift+C (always), Ctrl+C (only with a selection, otherwise
			// it must reach the shell as SIGINT), Ctrl+Insert.
			const bool copyChord =
				(pKb->vkCode == 'C' && ctrl && shift) ||
				(pKb->vkCode == 'C' && ctrl && !shift && g_activeTermPanel->hasSelection()) ||
				(pKb->vkCode == VK_INSERT && ctrl);
			if (copyChord)
			{
				::PostMessage(hTermWnd, WM_COPY, 0, 0);
				return 1;
			}

			// Paste: Ctrl+V, Ctrl+Shift+V, Shift+Insert
			const bool pasteChord =
				(pKb->vkCode == 'V' && ctrl) ||
				(pKb->vkCode == VK_INSERT && shift && !ctrl);
			if (pasteChord)
			{
				::PostMessage(hTermWnd, WM_PASTE, 0, 0);
				return 1;
			}

			// Select all: Ctrl+Shift+A (Ctrl+A stays ^A / beginning-of-line)
			if (pKb->vkCode == 'A' && ctrl && shift)
			{
				::PostMessage(hTermWnd, WM_APP_TERM_SELECTALL, 0, 0);
				return 1;
			}

			g_activeTermPanel->sendVKeyToTerminal(pKb->vkCode);
			return 1; // eat the message so Scintilla doesn't also receive it
		}
	}
	return ::CallNextHookEx(g_termKbHook, nCode, wParam, lParam);
}

// Standard 16 terminal colors (approximate)
TerminalPanel::~TerminalPanel()
{
	terminate();
	if (_hFont)
		::DeleteObject(_hFont);
}

bool TerminalPanel::initConPty()
{
	TERM_LOG(L"initConPty: loading from kernel32.dll");
	HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
	if (!hKernel32) { TERM_LOG(L"initConPty: no kernel32 handle"); return false; }

	_pfnCreatePseudoConsole = (PFN_CreatePseudoConsole)::GetProcAddress(hKernel32, "CreatePseudoConsole");
	_pfnResizePseudoConsole = (PFN_ResizePseudoConsole)::GetProcAddress(hKernel32, "ResizePseudoConsole");
	_pfnClosePseudoConsole  = (PFN_ClosePseudoConsole)::GetProcAddress(hKernel32, "ClosePseudoConsole");

	TERM_LOG(L"initConPty: CreatePseudoConsole=%p Resize=%p Close=%p",
		_pfnCreatePseudoConsole, _pfnResizePseudoConsole, _pfnClosePseudoConsole);
	return _pfnCreatePseudoConsole && _pfnResizePseudoConsole && _pfnClosePseudoConsole;
}

bool TerminalPanel::createPseudoConsole(COORD size)
{
	TERM_LOG(L"createPseudoConsole: cols=%d rows=%d", size.X, size.Y);
	HANDLE inPipeRead, inPipeWrite;
	HANDLE outPipeRead, outPipeWrite;

	SECURITY_ATTRIBUTES sa = {};
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = TRUE;

	if (!::CreatePipe(&inPipeRead, &inPipeWrite, &sa, 0))
	{ TERM_LOG(L"createPseudoConsole: CreatePipe(in) failed, err=%lu", ::GetLastError()); return false; }
	if (!::CreatePipe(&outPipeRead, &outPipeWrite, &sa, 0))
	{
		TERM_LOG(L"createPseudoConsole: CreatePipe(out) failed, err=%lu", ::GetLastError());
		::CloseHandle(inPipeRead);
		::CloseHandle(inPipeWrite);
		return false;
	}

	HRESULT hr = _pfnCreatePseudoConsole(size, inPipeRead, outPipeWrite, 0, &_hPC);
	TERM_LOG(L"createPseudoConsole: CreatePseudoConsole hr=0x%08X hPC=%p", hr, _hPC);

	if (FAILED(hr))
	{
		::CloseHandle(inPipeRead);
		::CloseHandle(inPipeWrite);
		::CloseHandle(outPipeRead);
		::CloseHandle(outPipeWrite);
		return false;
	}

	// Keep the console-side endpoints alive until CreateProcess completes.
	// Microsoft documents that closing them between CreatePseudoConsole and
	// CreateProcess can break the channel while the hosted process attaches.
	_ptyInputReadSide = inPipeRead;
	_ptyOutputWriteSide = outPipeWrite;
	_ptyInput  = inPipeWrite;   // we write here -> goes to process stdin
	_ptyOutput = outPipeRead;   // process stdout -> we read from here

	return true;
}

bool TerminalPanel::startProcess(const std::wstring& cmdLine, const std::wstring& workingDir)
{
	TERM_LOG(L"startProcess: cmd='%s' dir='%s'", cmdLine.c_str(), workingDir.c_str());
	STARTUPINFOEXW siEx = {};
	siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
	siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	siEx.StartupInfo.hStdInput = _ptyInputReadSide;
	siEx.StartupInfo.hStdOutput = _ptyOutputWriteSide;
	siEx.StartupInfo.hStdError = _ptyOutputWriteSide;

	// Set up the PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
	SIZE_T attrListSize = 0;

	// First call to get the size
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
	siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)::HeapAlloc(::GetProcessHeap(), 0, attrListSize);
	if (!siEx.lpAttributeList) return false;

	if (!::InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrListSize))
	{
		::HeapFree(::GetProcessHeap(), 0, siEx.lpAttributeList);
		return false;
	}

	if (!::UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
		PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, _hPC, sizeof(HPCON), nullptr, nullptr))
	{
		::DeleteProcThreadAttributeList(siEx.lpAttributeList);
		::HeapFree(::GetProcessHeap(), 0, siEx.lpAttributeList);
		return false;
	}

	// Prepare the command line (CreateProcess may modify it)
	wchar_t cmdCopy[4096] = {};
	wcscpy_s(cmdCopy, cmdLine.c_str());

	PROCESS_INFORMATION pi = {};

	const wchar_t* dir = workingDir.empty() ? nullptr : workingDir.c_str();

	// Build environment block with TERM=xterm-256color (like VS Code does)
	// to ensure ANSI escape sequences work correctly in the terminal.
	// The environment block is a series of null-terminated KEY=VALUE strings
	// with an extra null terminator at the end.
	void* envBlock = nullptr;
	{
		// Get current environment block
		wchar_t* curEnv = ::GetEnvironmentStringsW();
		if (curEnv)
		{
			// Calculate total size needed for current env + our additions
			size_t totalLen = 0;
			wchar_t* p = curEnv;
			while (*p)
			{
				totalLen += wcslen(p) + 1;  // string + null
				p += wcslen(p) + 1;
			}
			totalLen += 1;  // final null terminator

			// Add new variables
			const wchar_t* extraVars[] = {
				L"TERM=xterm-256color",
				L"TERM_PROGRAM=npp-terminal"
			};
			size_t extraLen = 0;
			for (const wchar_t* v : extraVars)
				extraLen += wcslen(v) + 1;

			// Allocate and copy
			size_t blockSize = (totalLen + extraLen) * sizeof(wchar_t);
			envBlock = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, blockSize);
			if (envBlock)
			{
				wchar_t* dst = static_cast<wchar_t*>(envBlock);
				// Copy existing environment
				p = curEnv;
				while (*p)
				{
					size_t len = wcslen(p) + 1;
					wcscpy_s(dst, len, p);
					dst += len;
					p += len;
				}
				// Append our variables
				for (const wchar_t* v : extraVars)
				{
					size_t len = wcslen(v) + 1;
					wcscpy_s(dst, len, v);
					dst += len;
				}
				*dst = L'\0';  // final null
			}
			::FreeEnvironmentStringsW(curEnv);
		}
	}

	// Diagnostic: inherit the parent environment directly. This isolates the
	// ConPTY/process setup from the hand-built environment block above.
	BOOL success = ::CreateProcessW(
		nullptr,          // lpApplicationName
		cmdCopy,          // lpCommandLine
		nullptr,          // lpProcessAttributes
		nullptr,          // lpThreadAttributes
		FALSE,            // bInheritHandles
		EXTENDED_STARTUPINFO_PRESENT,
		nullptr,          // lpEnvironment (inherit parent environment)
		dir,              // lpCurrentDirectory
		&siEx.StartupInfo, // lpStartupInfo
		&pi               // lpProcessInformation
	);

	// Diagnostic: retain the console-side endpoints for the full session.
	// If the shell now stays alive, its previous clean exit was caused by the
	// input channel observing EOF during/after pseudoconsole attachment.

	TERM_LOG(L"startProcess: CreateProcess result=%d err=%lu pid=%lu",
		success, success ? 0 : ::GetLastError(), success ? pi.dwProcessId : 0);

	if (envBlock)
		::HeapFree(::GetProcessHeap(), 0, envBlock);

	::DeleteProcThreadAttributeList(siEx.lpAttributeList);
	::HeapFree(::GetProcessHeap(), 0, siEx.lpAttributeList);

	if (!success) return false;

	::CloseHandle(pi.hThread);
	_hProcess = pi.hProcess;
	return true;
}

void TerminalPanel::resizePseudoConsole(SHORT cols, SHORT rows)
{
	if (_hPC && _pfnResizePseudoConsole)
	{
		COORD size = { cols, rows };
		_pfnResizePseudoConsole(_hPC, size);
	}
}

void TerminalPanel::launchShell(const std::wstring& shellPath, const std::wstring& workingDir)
{
	TERM_LOG(L"launchShell: path='%s' dir='%s'", shellPath.c_str(), workingDir.c_str());
	if (_running) { TERM_LOG(L"launchShell: terminating previous shell"); terminate(); }

	if (!initConPty())
	{
		TERM_LOG(L"launchShell: ConPTY not available!");
		::MessageBoxW(_hSelf, L"ConPTY not available. Requires Windows 10 version 1809 or later.",
			L"Terminal", MB_OK | MB_ICONERROR);
		return;
	}

	// Size the pseudo console to the actual panel size instead of a fixed
	// 80x24. When they disagreed, the shell wrapped its prompt at column 80
	// while we rendered a wider grid, which garbled redraws in PowerShell.
	if (_hTermWnd && _charWidth > 0 && _charHeight > 0)
	{
		RECT rc;
		::GetClientRect(_hTermWnd, &rc);
		int c = (rc.right - rc.left) / _charWidth;
		int r = (rc.bottom - rc.top) / _charHeight;
		_cols = static_cast<SHORT>(c >= 20 ? c : 80);
		_rows = static_cast<SHORT>(r >= 5 ? r : 24);
	}
	else
	{
		_cols = 80;
		_rows = 24;
	}

	if (!createPseudoConsole({ _cols, _rows }))
	{
		TERM_LOG(L"launchShell: createPseudoConsole failed!");
		::MessageBoxW(_hSelf, L"Failed to create pseudo console.",
			L"Terminal", MB_OK | MB_ICONERROR);
		return;
	}

	if (!startProcess(shellPath, workingDir))
	{
		TERM_LOG(L"launchShell: startProcess failed!");
		_pfnClosePseudoConsole(_hPC);
		_hPC = nullptr;
		::CloseHandle(_ptyInput);  _ptyInput = nullptr;
		::CloseHandle(_ptyOutput); _ptyOutput = nullptr;
		if (_ptyInputReadSide) { ::CloseHandle(_ptyInputReadSide); _ptyInputReadSide = nullptr; }
		if (_ptyOutputWriteSide) { ::CloseHandle(_ptyOutputWriteSide); _ptyOutputWriteSide = nullptr; }
		::MessageBoxW(_hSelf, L"Failed to launch shell process.",
			L"Terminal", MB_OK | MB_ICONERROR);
		return;
	}

	// Initialize text buffer
	{
		std::lock_guard<std::recursive_mutex> lock(_bufMutex);
		_buffer.clear();
		resetANSIParser();
		clearSelection();
		_cursorCol = 0;
		_cursorRow = 0;
		_screenTop = 0;
		_scrollTop = 0;
		_decCursorSaved = false;
		_autoScroll = true;
		ensureRow(0);
	}
	updateScrollBar();

	// Start reader thread
	TERM_LOG(L"launchShell: starting reader thread...");
	_running = true;
	_readerThread = std::thread(&TerminalPanel::readerThreadProc, this);
	TERM_LOG(L"launchShell: reader thread started, pid=%lu", ::GetProcessId(_hProcess));

	// ConPTY's conhost.exe keeps the output pipe open even after the child
	// shell process dies, so ReadFile() in readerThreadProc never sees EOF
	// and we never learn the shell exited (the panel just goes silent).
	// Watch the process handle directly so a crash/early-exit is diagnosable.
	_watchThread = std::thread(&TerminalPanel::watchProcessExit, this);

	// Install keyboard hook to capture keys before Scintilla.
	// The hook checks GetFocus() at runtime — only eats keys when the
	// terminal child window (or its parent dialog) has keyboard focus.
	installKbHook();

	// Show the window
	::ShowWindow(_hTermWnd, SW_SHOW);
}

void TerminalPanel::watchProcessExit()
{
	HANDLE hProc = _hProcess;
	if (!hProc) return;
	DWORD waitResult = ::WaitForSingleObject(hProc, INFINITE);
	if (waitResult == WAIT_OBJECT_0)
	{
		DWORD exitCode = 0;
		::GetExitCodeProcess(hProc, &exitCode);
		TERM_LOG(L"watchProcessExit: shell process exited, exitCode=%lu (0x%08lX)", exitCode, exitCode);
	}
	else
	{
		TERM_LOG(L"watchProcessExit: WaitForSingleObject returned %lu, err=%lu", waitResult, ::GetLastError());
	}
}

void TerminalPanel::readerThreadProc()
{
	TERM_LOG(L"readerThread: started, reading from PTY...");
	char buf[16384];
	DWORD bytesRead = 0;
	DWORD lastPaint = 0;

	while (_running)
	{
		if (!::ReadFile(_ptyOutput, buf, sizeof(buf), &bytesRead, nullptr) || bytesRead == 0)
		{
			DWORD err = ::GetLastError();
			TERM_LOG(L"readerThread: ReadFile returned 0, err=%lu, breaking", err);
			break;
		}

		processOutput(buf, bytesRead);

		// Coalesce repaints: invalidating on every chunk starved the UI thread
		// during bursty output (e.g. `dir` on a large tree, build logs).
		// InvalidateRect is cheap, but at ~60 Hz it is cheap *and* smooth.
		if (_hTermWnd)
		{
			DWORD now = ::GetTickCount();
			DWORD avail = 0;
			bool morePending = ::PeekNamedPipe(_ptyOutput, nullptr, 0, nullptr, &avail, nullptr)
				&& avail > 0;
			if (!morePending || now - lastPaint >= 16)
			{
				lastPaint = now;
				::InvalidateRect(_hTermWnd, nullptr, FALSE);
				// Keep the scrollbar range in sync from the UI thread.
				::PostMessage(_hTermWnd, WM_APP_TERM_SYNCSCROLL, 0, 0);
			}
		}
	}

	// Process exited
	{
		DWORD exitCode = 0;
		if (_hProcess && ::GetExitCodeProcess(_hProcess, &exitCode))
			TERM_LOG(L"readerThread: process exited, exitCode=%lu (0x%08lX)", exitCode, exitCode);
		else
			TERM_LOG(L"readerThread: process exited, exitCode unavailable");
	}
	_running = false;
	if (_hTermWnd)
		::InvalidateRect(_hTermWnd, nullptr, FALSE);
}

void TerminalPanel::terminate()
{
	TERM_LOG(L"terminate: entered, running=%d hProcess=%p hPC=%p", _running.load() ? 1 : 0, _hProcess, _hPC);
	uninstallKbHook();
	_running = false;

	// Close ConPTY and process BEFORE joining reader thread.
	// If we join first, ReadFile() in the reader thread blocks forever
	// because the child process is still running waiting for input.
	// Closing _hPC causes ReadFile to fail, unblocking the reader.
	if (_hPC)
	{
		_pfnClosePseudoConsole(_hPC);
		_hPC = nullptr;
	}

	{
		std::lock_guard<std::mutex> inputLock(_inputMutex);
		if (_ptyInput) { ::CloseHandle(_ptyInput); _ptyInput = nullptr; }
	}

	if (_hProcess)
	{
		TERM_LOG(L"terminate: calling TerminateProcess pid=%lu", ::GetProcessId(_hProcess));
		::TerminateProcess(_hProcess, 0);
		// TerminateProcess above makes the WaitForSingleObject in
		// watchProcessExit return almost immediately; join it before
		// closing the handle it is waiting on.
		if (_watchThread.joinable())
			_watchThread.join();
		::CloseHandle(_hProcess);
		_hProcess = nullptr;
	}
	if (_ptyInputReadSide) { ::CloseHandle(_ptyInputReadSide); _ptyInputReadSide = nullptr; }
	if (_ptyOutputWriteSide) { ::CloseHandle(_ptyOutputWriteSide); _ptyOutputWriteSide = nullptr; }
	else if (_watchThread.joinable())
	{
		_watchThread.join();
	}

	if (_readerThread.joinable())
		_readerThread.join();

	if (_ptyOutput) { ::CloseHandle(_ptyOutput); _ptyOutput = nullptr; }
}

void TerminalPanel::ensureRow(size_t row)
{
	while (row >= _buffer.size())
	{
		Line newLine;
		newLine.cells.resize(_cols, { L' ', _currentFg, _currentBg });
		_buffer.push_back(newLine);
	}
	if (_buffer[row].cells.size() < static_cast<size_t>(_cols))
		_buffer[row].cells.resize(_cols, { L' ', _currentFg, _currentBg });
}

void TerminalPanel::newLine()
{
	_cursorRow++;
	ensureRow(_cursorRow);

	// Once the cursor drops below the bottom of the visible screen, a real
	// terminal scrolls the screen up by one line. _screenTop must advance to
	// match — it is the baseline that absolute cursor positioning (CUP '[H'
	// and VPA '[d') is measured from. Without this, _screenTop stayed stuck
	// at its old value once output exceeded one screenful (e.g. `ls` on a
	// large directory), so PSReadLine's prompt-redraw CUP sequences kept
	// resolving to stale rows further up the scrollback and overwrote /
	// overlaid earlier output instead of the new bottom line.
	const size_t rows = static_cast<size_t>(_rows > 0 ? _rows : 24);
	if (_cursorRow >= _screenTop + rows)
		_screenTop = _cursorRow - rows + 1;

	while (_buffer.size() > _maxLines)
	{
		_buffer.pop_front();
		if (_cursorRow > 0) _cursorRow--;
		if (_screenTop > 0) _screenTop--;
		if (_scrollTop > 0) _scrollTop--;
		// Selection anchors shift with the buffer so the highlight doesn't
		// silently drift onto different text while output scrolls.
		if (_selStartCol >= 0)
		{
			if (_selStartRow > 0) _selStartRow--; else _selStartCol = -1;
			if (_selEndRow > 0)   _selEndRow--;   else _selEndCol = -1;
		}
	}
}

void TerminalPanel::putChar(wchar_t wc)
{
	// Auto-wrap at the right margin — without this, long lines (very common
	// in PowerShell output and in long paths) were truncated at _cols.
	if (_cursorCol >= _cols)
	{
		_cursorCol = 0;
		newLine();
	}
	ensureRow(_cursorRow);
	if (_cursorCol < static_cast<int>(_buffer[_cursorRow].cells.size()))
	{
		_buffer[_cursorRow].cells[_cursorCol] = { wc, _currentFg, _currentBg };
		_cursorCol++;
	}
}

void TerminalPanel::processOutput(const char* data, DWORD len)
{
	if (termLogEnabled())
	{
		std::wstring dump;
		dump.reserve(len * 4);
		for (DWORD i = 0; i < len; ++i)
		{
			unsigned char c = static_cast<unsigned char>(data[i]);
			if (c == 0x1B) dump += L"\\e";
			else if (c == L'\r') dump += L"\\r";
			else if (c == L'\n') dump += L"\\n\n";
			else if (c < 0x20 || c == 0x7F)
			{
				wchar_t hb[8];
				_snwprintf(hb, 8, L"\\x%02X", c);
				dump += hb;
			}
			else if (c < 0x80) dump += static_cast<wchar_t>(c);
			else
			{
				wchar_t hb[8];
				_snwprintf(hb, 8, L"\\x%02X", c);
				dump += hb;
			}
		}
		TERM_LOG(L"RAW[%lu]: %ls", len, dump.c_str());
	}
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);

	const char* p = data;
	const char* end = data + len;

	while (p < end)
	{
		unsigned char c = static_cast<unsigned char>(*p);

		switch (_ansiState)
		{
		case TS_NORMAL:
			// --- Incremental UTF-8 decoding ---
			// Previously each byte was cast straight to wchar_t, so every
			// non-ASCII character (box drawing, accents, CJK, and the arrows
			// PSReadLine emits) was rendered as mojibake. A byte sequence can
			// also straddle two ReadFile chunks, hence the persistent state.
			if (_utf8Remaining > 0)
			{
				if ((c & 0xC0) == 0x80)
				{
					_utf8Acc = (_utf8Acc << 6) | (c & 0x3F);
					if (--_utf8Remaining == 0)
					{
						if (_utf8Acc <= 0xFFFF)
						{
							putChar(static_cast<wchar_t>(_utf8Acc));
						}
						else if (_utf8Acc <= 0x10FFFF)
						{
							// Encode as a UTF-16 surrogate pair
							unsigned int v = _utf8Acc - 0x10000;
							putChar(static_cast<wchar_t>(0xD800 + (v >> 10)));
							putChar(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
						}
						_utf8Acc = 0;
					}
					++p;
					continue;
				}
				// Malformed sequence — drop it and reinterpret this byte.
				_utf8Remaining = 0;
				_utf8Acc = 0;
			}

			if (c == 0x1B) // ESC
			{
				_ansiState = TS_ESCAPE;
				_ansiParams.clear();
				_ansiQuestionMark = false;
				_ansiDollarIntermediate = false;
			}
			else if (c == '\r')
			{
				_cursorCol = 0;
			}
			else if (c == '\n')
			{
				_cursorCol = 0;
				newLine();
			}
			else if (c == '\b')
			{
				if (_cursorCol > 0) _cursorCol--;
			}
			else if (c == '\t')
			{
				int target = ((_cursorCol / 8) + 1) * 8;
				if (target > _cols) target = _cols;
				while (_cursorCol < target)
					putChar(L' ');
			}
			else if (c == 0x07) // BEL - ignore
			{
			}
			else if (c < 0x20 || c == 0x7F)
			{
				// Other C0 controls / DEL: ignore
			}
			else if (c < 0x80)
			{
				putChar(static_cast<wchar_t>(c));
			}
			else if ((c & 0xE0) == 0xC0) { _utf8Acc = c & 0x1F; _utf8Remaining = 1; }
			else if ((c & 0xF0) == 0xE0) { _utf8Acc = c & 0x0F; _utf8Remaining = 2; }
			else if ((c & 0xF8) == 0xF0) { _utf8Acc = c & 0x07; _utf8Remaining = 3; }
			// else: invalid lead byte, ignore
			break;

		case TS_ESCAPE:
			if (c == '[')
			{
				_ansiState = TS_CSI;
			}
			else if (c == ']')
			{
				_ansiState = TS_OSC;
				_ansiParams.clear();
				_ansiOscEsc = false;
			}
			else if (c == '(' || c == ')' || c == '*' || c == '+')
			{
				_ansiState = TS_CHARSET;   // consume the charset designator byte
			}
			else if (c == 'M')  // RI — reverse index (cursor up, scroll if needed)
			{
				if (_cursorRow > 0) _cursorRow--;
				_ansiState = TS_NORMAL;
			}
			else if (c == 'D')  // IND — index (cursor down)
			{
				newLine();
				_ansiState = TS_NORMAL;
			}
			else if (c == 'c')  // RIS — full reset
			{
				resetANSIParser();
				_ansiState = TS_NORMAL;
			}
			else
			{
				_ansiState = TS_NORMAL;
			}
			break;

		case TS_CHARSET:
			_ansiState = TS_NORMAL;
			break;

		case TS_CSI:
			if (c >= '0' && c <= '9')
			{
				if (_ansiParams.size() < 128) _ansiParams += c;
			}
			else if (c == ';' || c == ':')
			{
				if (_ansiParams.size() < 128) _ansiParams += ';';
			}
			else if (c == '?' || c == '>' || c == '<' || c == '=')
			{
				_ansiQuestionMark = (c == '?');
				_ansiPrefixChar = static_cast<char>(c);
			}
			else if (c == '$')
			{
				_ansiDollarIntermediate = true;
			}
			else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '`')
			{
				_ansiFinalByte = c;
				handleANSIEscape();
				_ansiState = TS_NORMAL;
				_ansiPrefixChar = 0;
				_ansiDollarIntermediate = false;
			}
			// else ignore intermediate bytes
			break;

		case TS_OSC:
			// Terminated by BEL or by ST (ESC backslash). The old check
			// treated any bare backslash inside the payload (e.g. a Windows
			// path in a title sequence) as the terminator, so the rest of the
			// title leaked into the visible output.
			if (c == 0x07)
			{
				handleOSC();
				_ansiState = TS_NORMAL;
			}
			else if (_ansiOscEsc)
			{
				_ansiState = TS_NORMAL;   // ESC \ (ST) or malformed — end it
				if (c != '\\')
				{
					continue;               // reinterpret this byte
				}
				handleOSC();
			}
			else if (c == 0x1B)
			{
				_ansiOscEsc = true;
			}
			else
			{
				if (_ansiParams.size() < 1024)   // bound the payload
					_ansiParams += c;
			}
			break;
		}

		++p;
	}

	if (_autoScroll)
	{
		const int rows = _rows > 0 ? _rows : 24;
		size_t maxTop = (_buffer.size() > static_cast<size_t>(rows))
			? _buffer.size() - rows : 0;
		_scrollTop = maxTop;
	}
}

void TerminalPanel::handleANSIEscape()
{
	char cmd = _ansiFinalByte;

	// Parse params with saturation; malformed escape sequences must not invoke
	// signed-overflow behavior or allocate unbounded parser state.
	std::vector<int> params;
	if (!_ansiParams.empty())
	{
		const char* ps = _ansiParams.c_str();
		while (*ps)
		{
			if (*ps >= '0' && *ps <= '9')
			{
				int value = 0;
				while (*ps >= '0' && *ps <= '9')
				{
					const int digit = *ps - '0';
					value = value > (INT_MAX - digit) / 10 ? INT_MAX : value * 10 + digit;
					++ps;
				}
				params.push_back(value);
			}
			else if (*ps == ';')
			{
				++ps;
			}
			else
			{
				++ps;
			}
		}
	}
	// Default parameter when omitted differs by command:
	//   - SGR ('m') and the erase commands ED ('J') / EL ('K') default to 0.
	//   - Cursor-movement commands (A/B/C/D/etc.) default to 1 (move by one).
	// The old code defaulted everything except 'm' to 1, so a bare "\e[K"
	// (very common — PowerShell/PSReadLine emit it after every colorized
	// filename, e.g. directory listings) was treated as "\e[1K" (erase from
	// line start to cursor) instead of "\e[0K" (erase from cursor to line
	// end). That wiped out the text that had just been printed, turning
	// colorized entries (directories, symlinks) into blank lines — exactly
	// what showed up as missing/blank rows in `ls`/`ls -al` output.
	if (params.empty())
		params.push_back((cmd == 'm' || cmd == 'J' || cmd == 'K') ? 0 : 1);

	switch (cmd)
	{
	case 'm': // SGR - Select Graphic Rendition
		// Private SGR modes, such as CSI > 4 ; 1 m from opentui, are
		// capability commands rather than text attributes.
		if (!_ansiPrefixChar)
			processSGR();
		break;

	case 'A': // Cursor Up
		if (params[0] == 0) params[0] = 1;
		_cursorRow = (_cursorRow >= (size_t)params[0]) ? (_cursorRow - params[0]) : 0;
		break;

	case 'B': // Cursor Down
	case 'e':
		if (params[0] == 0) params[0] = 1;
		_cursorRow += params[0];
		ensureRow(_cursorRow);
		break;

	case 'C': // Cursor Forward
	case 'a':
		if (params[0] == 0) params[0] = 1;
		_cursorCol += params[0];
		if (_cursorCol >= _cols) _cursorCol = _cols - 1;
		break;

	case 'D': // Cursor Back
		if (params[0] == 0) params[0] = 1;
		_cursorCol = (_cursorCol >= params[0]) ? (_cursorCol - params[0]) : 0;
		break;

	case 'E': // Cursor Next Line
		if (params[0] == 0) params[0] = 1;
		_cursorRow += params[0];
		_cursorCol = 0;
		ensureRow(_cursorRow);
		break;

	case 'F': // Cursor Previous Line
		if (params[0] == 0) params[0] = 1;
		_cursorRow = (_cursorRow >= (size_t)params[0]) ? (_cursorRow - params[0]) : 0;
		_cursorCol = 0;
		break;

	case 'G': // CHA - Cursor Horizontal Absolute
	case '`':
		_cursorCol = (params[0] > 0) ? params[0] - 1 : 0;
		if (_cursorCol >= _cols) _cursorCol = _cols - 1;
		break;

	case 'd': // VPA - Line Position Absolute (screen-relative)
		_cursorRow = _screenTop + ((params[0] > 0) ? params[0] - 1 : 0);
		ensureRow(_cursorRow);
		break;

	case 'H': // Cursor Position
	case 'f':
		{
			int row = (params.size() >= 1 && params[0] > 0) ? params[0] - 1 : 0;
			int col = (params.size() >= 2 && params[1] > 0) ? params[1] - 1 : 0;
			// CUP is relative to the top of the *screen*, not the top of the
			// scrollback. Using an absolute buffer index (the old behaviour)
			// made every full-screen redraw — including the PowerShell prompt
			// repaint after each keystroke — overwrite the scrollback at
			// buffer row 0, which looked like the terminal "eating" output.
			_cursorRow = _screenTop + static_cast<size_t>(row);
			_cursorCol = (col < _cols) ? col : _cols - 1;
			ensureRow(_cursorRow);
		}
		break;

	case 'J': // Erase in Display
		if (params[0] == 2 || params[0] == 3)
		{
			// Clear the visible screen. For ED 3 also drop the scrollback.
			if (params[0] == 3)
			{
				_buffer.clear();
				_screenTop = 0;
				_scrollTop = 0;
				clearSelection();
				ensureRow(0);
				_cursorRow = 0;
				_cursorCol = 0;
			}
			else
			{
				// Scroll the screen away instead of destroying history so
				// `cls` / Clear-Host doesn't wipe the user's scrollback.
				_screenTop = _buffer.size();
				ensureRow(_screenTop + (_rows > 0 ? _rows - 1 : 0));
				_cursorRow = _screenTop;
				_cursorCol = 0;
				_scrollTop = _screenTop;  // keep viewport in sync with new screen
				_autoScroll = true;
			}
		}
		else if (params[0] == 0)
		{
			// Clear from cursor to end of screen
			if (_cursorRow < _buffer.size())
			{
				for (int c = _cursorCol; c < _cols; c++)
				{
					if (c < (int)_buffer[_cursorRow].cells.size())
						_buffer[_cursorRow].cells[c] = { L' ', _currentFg, _currentBg };
				}
				for (size_t r = _cursorRow + 1; r < _buffer.size(); r++)
				{
					for (int c = 0; c < _cols && c < (int)_buffer[r].cells.size(); c++)
						_buffer[r].cells[c] = { L' ', _currentFg, _currentBg };
				}
			}
		}
		else if (params[0] == 1)
		{
			// Clear from beginning of screen to cursor
			if (_cursorRow < _buffer.size())
			{
				for (size_t r = _screenTop; r < _cursorRow && r < _buffer.size(); r++)
				{
					for (int c = 0; c < _cols && c < (int)_buffer[r].cells.size(); c++)
						_buffer[r].cells[c] = { L' ', _currentFg, _currentBg };
				}
				for (int c = 0; c <= _cursorCol && c < (int)_buffer[_cursorRow].cells.size(); c++)
					_buffer[_cursorRow].cells[c] = { L' ', _currentFg, _currentBg };
			}
		}
		break;

	case 'K': // Erase in Line
		if (_cursorRow < _buffer.size())
		{
			int start = (params[0] == 1) ? 0 : ((params[0] == 2) ? 0 : _cursorCol);
			int endCol = (params[0] == 1) ? (_cursorCol + 1) : _cols;
			for (int c = start; c < endCol && c < (int)_buffer[_cursorRow].cells.size(); c++)
				_buffer[_cursorRow].cells[c] = { L' ', _currentFg, _currentBg };
		}
		break;

	case 'X': // ECH - Erase Character
		if (_cursorRow < _buffer.size())
		{
			int n = (params[0] > 0) ? params[0] : 1;
			for (int c = _cursorCol; c < _cursorCol + n && c < (int)_buffer[_cursorRow].cells.size(); c++)
				_buffer[_cursorRow].cells[c] = { L' ', _currentFg, _currentBg };
		}
		break;

	case 'P': // DCH - Delete Character (shift the rest of the line left)
		if (_cursorRow < _buffer.size())
		{
			int n = (params[0] > 0) ? params[0] : 1;
			auto& cells = _buffer[_cursorRow].cells;
			for (int c = _cursorCol; c < (int)cells.size(); c++)
			{
				int src = c + n;
				cells[c] = (src < (int)cells.size()) ? cells[src]
					: Cell{ L' ', _currentFg, _currentBg };
			}
		}
		break;

	case '@': // ICH - Insert Character
		if (_cursorRow < _buffer.size())
		{
			int n = (params[0] > 0) ? params[0] : 1;
			auto& cells = _buffer[_cursorRow].cells;
			for (int c = (int)cells.size() - 1; c >= _cursorCol; c--)
			{
				int src = c - n;
				cells[c] = (src >= _cursorCol) ? cells[src]
					: Cell{ L' ', _currentFg, _currentBg };
			}
		}
		break;

	case 'h': // Set Mode (DECSET when prefixed by '?')
		if (_ansiQuestionMark)
		{
			for (int prm : params)
			{
				if (prm == 25)        _cursorVisible = true;
				else if (prm == 2004) _bracketedPaste = true;   // enable bracketed paste
				else if (prm == 1004) _focusReporting = true;
				else if (prm == 1)    _appCursorKeys = true;    // DECCKM
				else if (prm == 1049 || prm == 47 || prm == 1047)
					enterAltScreen();   // full-screen TUI apps (vim, opencode, htop, ...)
			}
		}
		break;

	case 'l': // Reset Mode (DECRST when prefixed by '?')
		if (_ansiQuestionMark)
		{
			for (int prm : params)
			{
				if (prm == 25)        _cursorVisible = false;
				else if (prm == 2004) _bracketedPaste = false;
				else if (prm == 1004) _focusReporting = false;
				else if (prm == 1)    _appCursorKeys = false;
				else if (prm == 1049 || prm == 47 || prm == 1047)
					leaveAltScreen();
			}
		}
		break;

	case 'n': // DSR - Device Status Report
		if (!_ansiQuestionMark && !params.empty() && (params[0] == 5 || params[0] == 6))
		{
			if (params[0] == 5)
			{
				// DSR status request: report that the terminal is operational.
				TERM_LOG(L"handleANSI: answering DSR 5");
				sendTextToTerminal("\x1b[0n");
			}
			else
			{
				// DSR cursor position request. OpenTUI uses this during startup
				// before and after switching to the alternate screen.
				int row = static_cast<int>(_cursorRow >= _screenTop ? _cursorRow - _screenTop : 0) + 1;
				int col = _cursorCol + 1;
				TERM_LOG(L"handleANSI: answering DSR 6 row=%d col=%d", row, col);
				sendTextToTerminal("\x1b[" + std::to_string(row) + ";" +
					std::to_string(col) + "R");
			}
		}
		break;

	case 'p': // DECRQM - request DEC private mode report
		if (_ansiQuestionMark && _ansiDollarIntermediate)
		{
			TERM_LOG(L"handleANSI: DECRQM params='%hs'", _ansiParams.c_str());
			// OpenTUI probes private modes with CSI ? Pm $ p. Report the modes
			// this panel supports using the corresponding DECRPM response.
			for (int prm : params)
			{
				const bool supported = prm == 25 || prm == 1000 || prm == 1002 ||
					prm == 1003 || prm == 1004 || prm == 1006 || prm == 1049 ||
					prm == 2004 || prm == 2026 || prm == 2027;
				sendTextToTerminal("\x1b[?" + std::to_string(prm) + ";" +
					(supported ? "2" : "0") + "$y");
			}
		}
		break;

	case 'c': // DA - Device Attributes (bare CSI c = DA1, CSI > c = DA2)
		// Many TUI frameworks (opencode's opentui, and other Rust/Go/JS
		// terminal libs) send a DA query as part of capability detection
		// and gate real rendering on receiving a response. Without any
		// reply the app can appear to hang on a blank screen forever.
		if (_ansiPrefixChar == '>')
		{
			// DA2: "I am terminal type;firmware version;keyboard 0"
			sendTextToTerminal("\x1b[>0;10;0c");
		}
		else
		{
			// DA1: claim basic VT220 compliance (no special features)
			sendTextToTerminal("\x1b[?62c");
		}
		break;

	case 'u': // Kitty keyboard protocol query (CSI ? u) / report (CSI <flags> u)
		if (_ansiQuestionMark)
		{
			// "What keyboard protocol flags are active?" — reply with 0
			// (no enhanced flags) so apps that gate on this response (e.g.
			// opencode/opentui) stop blocking and fall back to legacy input.
			sendTextToTerminal("\x1b[?0u");
		}
		else if (!_ansiQuestionMark && !_ansiPrefixChar && _decCursorSaved)
		{
			// DECRC: restore the cursor saved by CSI s.
			_cursorRow = _decSavedCursorRow;
			_cursorCol = _decSavedCursorCol;
			ensureRow(_cursorRow);
		}
		else if (_ansiPrefixChar == '>')
		{
			// Accept Kitty keyboard protocol commands as a no-op. Input remains
			// in the legacy mode supported by this terminal panel.
		}
		break;

	case 's': // DECSC - save cursor position
		_decSavedCursorRow = _cursorRow;
		_decSavedCursorCol = _cursorCol;
		_decCursorSaved = true;
		break;

	case 'q': // DECSCUSR or XTVERSION (CSI > q)
		if (_ansiPrefixChar == '>')
		{
			// OpenTUI uses XTVERSION during startup to identify the terminal
			// path. Return a valid DCS response so capability negotiation can
			// finish instead of waiting for the timeout.
			TERM_LOG(L"handleANSI: answering XTVERSION");
			sendTextToTerminal("\x1bP>|NodePlus Terminal 1.0\x1b\\");
		}
		// DECSCUSR cursor style is intentionally rendered using one native
		// cursor style.
		break;

	case 't': // XTWINOPS - window/report queries
		if (!_ansiQuestionMark && params.size() >= 1)
		{
			if (params[0] == 14)
			{
				// Report text-area size in pixels: CSI 4 ; height ; width t
				sendTextToTerminal("\x1b[4;" + std::to_string(_rows * _charHeight) + ";" +
					std::to_string(_cols * _charWidth) + "t");
			}
			else if (params[0] == 18)
			{
				// Report text-area size in characters: CSI 8 ; rows ; cols t
				sendTextToTerminal("\x1b[8;" + std::to_string(_rows) + ";" +
					std::to_string(_cols) + "t");
			}
			else if (params[0] == 19)
			{
				// Report screen size in characters: CSI 9 ; rows ; cols t
				sendTextToTerminal("\x1b[9;" + std::to_string(_rows) + ";" +
					std::to_string(_cols) + "t");
			}
		}
		break;
	}
}

void TerminalPanel::processSGR()
{
	// SGR needs to be handled as a sequence (not per-parameter) because the
	// 256-color / truecolor forms consume the following parameters:
	//   38;5;N        -> 256-color foreground
	//   38;2;R;G;B    -> truecolor foreground
	//   48;5;N / 48;2;R;G;B -> same for background
	// The old per-parameter loop ignored 38/48 and then applied the *following*
	// numbers as if they were standalone SGR codes, producing wrong colors.
	std::vector<int> params;
	const char* ps = _ansiParams.c_str();
	bool sawDigit = false;
	int cur = 0;
	for (;; ++ps)
	{
		if (*ps >= '0' && *ps <= '9')
		{
			cur = cur * 10 + (*ps - '0');
			sawDigit = true;
		}
		else
		{
			params.push_back(sawDigit ? cur : 0);   // empty param means 0
			cur = 0;
			sawDigit = false;
			if (*ps == '\0') break;
		}
	}
	if (params.empty()) params.push_back(0);

	auto xterm256 = [this](int idx) -> COLORREF {
		if (idx < 16) return _stdColors[idx];
		if (idx < 232)
		{
			int i = idx - 16;
			static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
			return RGB(lv[(i / 36) % 6], lv[(i / 6) % 6], lv[i % 6]);
		}
		int g = 8 + (idx - 232) * 10;
		if (g > 255) g = 255;
		return RGB(g, g, g);
	};

	for (size_t i = 0; i < params.size(); ++i)
	{
		const int prm = params[i];
		if (prm == 38 || prm == 48)
		{
			const bool fg = (prm == 38);
			if (i + 1 < params.size() && params[i + 1] == 5 && i + 2 < params.size())
			{
				COLORREF col = xterm256(params[i + 2]);
				if (fg) _currentFg = col; else _currentBg = col;
				i += 2;
			}
			else if (i + 1 < params.size() && params[i + 1] == 2 && i + 4 < params.size())
			{
				COLORREF col = RGB(params[i + 2] & 0xFF, params[i + 3] & 0xFF, params[i + 4] & 0xFF);
				if (fg) _currentFg = col; else _currentBg = col;
				i += 4;
			}
			continue;
		}
		applySGR(prm);
	}
}

void TerminalPanel::handleOSC()
{
	// _ansiParams holds the OSC payload, e.g. "0;title" or "4;0;?" or
	// "1337;Capabilities" or "99;i=...;p=?;...". Several TUI frameworks
	// (opencode's "opentui" stack among them) send OSC capability/color
	// queries during startup and gate their first real frame on getting
	// *some* reply. Without any response the app can sit forever on the
	// blank placeholder frame it already drew — exactly the "opencode
	// hangs blank" symptom. We don't implement these features, but we
	// must still answer so the app's probe completes and it moves on.
	const std::string& s = _ansiParams;
	size_t semi = s.find(';');
	if (semi == std::string::npos)
		return;

	std::string codeStr = s.substr(0, semi);
	std::string rest = s.substr(semi + 1);
	int code = 0;
	try { code = std::stoi(codeStr); } catch (...) { return; }

	if (code == 99 && rest.find("i=opentui-notifications") != std::string::npos &&
		rest.find("p=?") != std::string::npos)
	{
		// OpenTUI probes OSC 99 before enabling its notification capability.
		// Answer the probe so its startup capability detection can complete.
		TERM_LOG(L"handleOSC: replying to OpenTUI OSC 99 capability query");
		sendTextToTerminal("\x1b]99;i=opentui-notifications:p=?;p=title,body:o=always\x1b\\");
	}
	else if (code == 1337 && rest == "Capabilities")
	{
		// OpenTUI sends the iTerm2 capability query as part of startup. A
		// response is required for its capability-response parser to finish;
		// an empty feature list accurately advertises no iTerm extensions.
		TERM_LOG(L"handleOSC: replying to iTerm capability query");
		sendTextToTerminal("\x1b]1337;Capabilities=\x1b\\");
	}
	else if (code == 4)
	{
		// OSC 4 ; <index> ; ? — query palette color at <index>.
		// Reply: OSC 4 ; <index> ; rgb:RRRR/GGGG/BBBB BEL
		size_t semi2 = rest.find(';');
		if (semi2 != std::string::npos && rest.substr(semi2 + 1) == "?")
		{
			int idx = 0;
			try { idx = std::stoi(rest.substr(0, semi2)); } catch (...) { idx = 0; }
			COLORREF col = (idx >= 0 && idx < 16) ? _stdColors[idx] : _stdColors[0];
			wchar_t buf[64];
			_snwprintf(buf, 64, L"\x1b]4;%d;rgb:%02x%02x/%02x%02x/%02x%02x\x07",
				idx, GetRValue(col), GetRValue(col), GetGValue(col), GetGValue(col),
				GetBValue(col), GetBValue(col));
			std::wstring wbuf(buf);
			std::string out(wbuf.begin(), wbuf.end());
			sendTextToTerminal(out);
		}
	}
	else if (code == 10 || code == 11)
	{
		// OSC 10/11 ; ? — query default foreground/background color.
		if (rest == "?")
		{
			COLORREF col = (code == 10) ? _stdColors[7] : _stdColors[0];
			wchar_t buf[48];
			_snwprintf(buf, 48, L"\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x07", code,
				GetRValue(col), GetRValue(col), GetGValue(col), GetGValue(col),
				GetBValue(col), GetBValue(col));
			std::wstring wbuf(buf);
			std::string out(wbuf.begin(), wbuf.end());
			sendTextToTerminal(out);
		}
	}
	// OSC 0/1/2 (window/icon title) and other cosmetic OSCs are simply
	// consumed above with no reply needed (real terminals don't answer
	// them either). OSC 99 (notifications), 1337 (iTerm2 capabilities),
	// and 66 (tab sizing hints) also don't require a written reply — they
	// are "fire and forget" hints from the app, not queries, so leaving
	// them unanswered is correct and matches real-terminal behavior.
}

void TerminalPanel::enterAltScreen()
{
	if (_inAltScreen) return;
	_inAltScreen = true;
	_savedBuffer = _buffer;
	_savedScrollTop = _scrollTop;
	_savedScreenTop = _screenTop;
	_savedCursorRow = _cursorRow;
	_savedCursorCol = _cursorCol;

	// Full-screen apps redraw from a blank canvas; starting the alt screen
	// at a fresh buffer (rather than continuing the main scrollback) keeps
	// their frame redraws from interleaving with prior shell output.
	_buffer.clear();
	_screenTop = 0;
	_scrollTop = 0;
	_cursorRow = 0;
	_cursorCol = 0;
	clearSelection();
	ensureRow(static_cast<size_t>(_rows > 0 ? _rows - 1 : 0));
}

void TerminalPanel::leaveAltScreen()
{
	if (!_inAltScreen) return;
	_inAltScreen = false;
	_buffer = _savedBuffer;
	_scrollTop = _savedScrollTop;
	_screenTop = _savedScreenTop;
	_cursorRow = _savedCursorRow;
	_cursorCol = _savedCursorCol;
	_savedBuffer.clear();
	clearSelection();
	_autoScroll = true;
}

void TerminalPanel::resetANSIParser()
{
	_ansiState = TS_NORMAL;
	_ansiParams.clear();
	_ansiFinalByte = 0;
	_ansiQuestionMark = false;
	_ansiPrefixChar = 0;
	_ansiOscEsc = false;
	_utf8Acc = 0;
	_utf8Remaining = 0;
	_currentFg = _stdColors[7];
	_currentBg = _stdColors[0];
	_bold = _underline = _inverse = false;
	_bracketedPaste = false;
	_appCursorKeys = false;
}

void TerminalPanel::applySGR(int param)
{
	switch (param)
	{
	case 0:  // Reset
		_currentFg = _stdColors[7];
		_currentBg = _stdColors[0];
		_bold = false;
		_underline = false;
		_inverse = false;
		break;
	case 1:  // Bold
		_bold = true;
		break;
	case 4:  // Underline
		_underline = true;
		break;
	case 7:  // Inverse
		_inverse = true;
		break;
	case 22: // Normal intensity
		_bold = false;
		break;
	case 24: // Underline off
		_underline = false;
		break;
	case 27: // Inverse off
		_inverse = false;
		break;
	case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
		_currentFg = _stdColors[param - 30];
		if (_bold) _currentFg = _stdColors[(param - 30) + 8];
		break;
	case 38: // Extended foreground - ignore for now
		break;
	case 39: // Default foreground
		_currentFg = _stdColors[7];
		break;
	case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
		_currentBg = _stdColors[param - 40];
		break;
	case 48: // Extended background - ignore for now
		break;
	case 49: // Default background
		_currentBg = _stdColors[0];
		break;
	case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
		_currentFg = _stdColors[(param - 90) + 8];
		break;
	case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
		_currentBg = _stdColors[(param - 100) + 8];
		break;
	}
}

void TerminalPanel::sendKeyToTerminal(WPARAM wParam, LPARAM lParam)
{
	// Kept for compatibility; all key translation now lives in
	// sendVKeyToTerminal so there is a single, layout-aware implementation.
	(void)lParam;
	sendVKeyToTerminal(static_cast<DWORD>(wParam));
}

void TerminalPanel::sendVKeyToTerminal(DWORD vkCode)
{
	if (!_running || !_ptyInput) return;

	// GetAsyncKeyState (not GetKeyState) — see comment in termKbHookProc.
	const bool ctrl  = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool alt   = (::GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

	// Any keystroke means the user wants to be at the prompt again.
	if (!_autoScroll)
		scrollToBottom();

	std::string out;

	// Build the CSI modifier parameter used by xterm: 1 + (shift?1) + (alt?2) + (ctrl?4)
	auto modParam = [&]() -> int {
		int m = 1;
		if (shift) m += 1;
		if (alt)   m += 2;
		if (ctrl)  m += 4;
		return m;
	};
	// Cursor/nav key: emits either "ESC [ <final>" or "ESC [ 1 ; <mod> <final>"
	auto csiKey = [&](char finalByte) {
		const int m = modParam();
		if (m == 1)
		{
			// DECCKM: application cursor keys use SS3 (ESC O A) instead of CSI
			if (_appCursorKeys && (finalByte == 'A' || finalByte == 'B' ||
			                       finalByte == 'C' || finalByte == 'D'))
				out = std::string("\x1b" "O") + finalByte;
			else
				out = std::string("\x1b" "[") + finalByte;
		}
		else
		{
			out = "\x1b[1;" + std::to_string(m) + finalByte;
		}
	};
	// Tilde-style key: "ESC [ <n> ~" or "ESC [ <n> ; <mod> ~"
	auto tildeKey = [&](int n) {
		const int m = modParam();
		out = "\x1b[" + std::to_string(n);
		if (m != 1) out += ";" + std::to_string(m);
		out += "~";
	};

	switch (vkCode)
	{
	case VK_RETURN: out = "\r"; break;
	case VK_TAB:    out = shift ? "\x1b[Z" : "\t"; break;
	case VK_BACK:   out = ctrl ? "\x08" : "\x7f"; break;   // DEL is the conventional BS
	case VK_ESCAPE: out = "\x1b"; break;

	// ---- Cursor / navigation keys ----
	// These were entirely missing before, which is why arrow-key command
	// history, Home/End line editing and Delete did nothing in PowerShell.
	case VK_UP:     csiKey('A'); break;
	case VK_DOWN:   csiKey('B'); break;
	case VK_RIGHT:  csiKey('C'); break;
	case VK_LEFT:   csiKey('D'); break;
	case VK_HOME:   csiKey('H'); break;
	case VK_END:    csiKey('F'); break;
	case VK_INSERT: tildeKey(2); break;
	case VK_DELETE: tildeKey(3); break;

	// PageUp/PageDown scroll our own scrollback when Shift is held,
	// otherwise they are forwarded to the shell.
	case VK_PRIOR:
		if (shift) { scrollTerm(-visibleRows() / 2); return; }
		tildeKey(5);
		break;
	case VK_NEXT:
		if (shift) { scrollTerm(visibleRows() / 2); return; }
		tildeKey(6);
		break;

	// ---- Function keys ----
	case VK_F1:  out = "\x1bOP"; break;
	case VK_F2:  out = "\x1bOQ"; break;
	case VK_F3:  out = "\x1bOR"; break;
	case VK_F4:  out = "\x1bOS"; break;
	case VK_F5:  tildeKey(15); break;
	case VK_F6:  tildeKey(17); break;
	case VK_F7:  tildeKey(18); break;
	case VK_F8:  tildeKey(19); break;
	case VK_F9:  tildeKey(20); break;
	case VK_F10: tildeKey(21); break;
	case VK_F11: tildeKey(23); break;
	case VK_F12: tildeKey(24); break;

	case VK_SPACE:
		out = ctrl ? std::string(1, '\0') : " ";   // Ctrl+Space -> NUL
		break;

	default:
	{
		// Ctrl+letter -> control character (^A = 0x01 .. ^Z = 0x1A)
		if (ctrl && !alt && vkCode >= 'A' && vkCode <= 'Z')
		{
			out.assign(1, static_cast<char>(vkCode - 'A' + 1));
			break;
		}
		// Ctrl+[ \ ] ^ _  -> 0x1B..0x1F
		if (ctrl && !alt)
		{
			switch (vkCode)
			{
			case VK_OEM_4: out.assign(1, 0x1b); break; // Ctrl+[
			case VK_OEM_5: out.assign(1, 0x1c); break; // Ctrl+backslash
			case VK_OEM_6: out.assign(1, 0x1d); break; // Ctrl+]
			default: break;
			}
			if (!out.empty()) break;
		}

		// Map the virtual key to text via the active keyboard layout so that
		// non-US layouts, dead keys and AltGr all behave correctly.
		BYTE keyState[256] = {};
		::GetKeyboardState(keyState);
		// GetKeyboardState can lag inside a low-level hook; patch in the real
		// modifier state so Shift-ed characters are produced correctly.
		keyState[VK_SHIFT]   = shift ? 0x80 : 0;
		keyState[VK_LSHIFT]  = shift ? 0x80 : 0;
		keyState[VK_CAPITAL] = static_cast<BYTE>(::GetKeyState(VK_CAPITAL) & 0x01);
		// Clear Ctrl so ToUnicode returns the printable char, not a control code.
		if (!ctrl)
		{
			keyState[VK_CONTROL] = 0;
			keyState[VK_LCONTROL] = 0;
			keyState[VK_RCONTROL] = 0;
		}

		wchar_t wbuf[8] = {};
		UINT scanCode = ::MapVirtualKeyW(vkCode, MAPVK_VK_TO_VSC);
		int result = ::ToUnicode(vkCode, scanCode, keyState, wbuf, 8, 0);
		if (result > 0)
		{
			char mb[16] = {};
			int len = ::WideCharToMultiByte(CP_UTF8, 0, wbuf, result, mb, sizeof(mb), nullptr, nullptr);
			for (int i = 0; i < len; ++i)
			{
				if (static_cast<unsigned char>(mb[i]) >= 0x20 || mb[i] == '\t')
					out += mb[i];
			}
		}
		break;
	}
	}

	if (out.empty()) return;

	// Alt+<key> is transmitted as ESC followed by the key (xterm meta convention).
	if (alt && out[0] != 0x1b)
		out.insert(out.begin(), '\x1b');

	sendTextToTerminal(out);
}

void TerminalPanel::sendBreakToProcess()
{
	// Do NOT use GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) here: process group
	// 0 means "every process attached to the *caller's* console", i.e. it can
	// signal Notepad++ itself. With ConPTY the correct way is to write ^C
	// (0x03) into the PTY input, which the pseudo console converts into a
	// proper Ctrl+C for the child process group.
	if (_running && _ptyInput)
	{
		sendTextToTerminal(std::string(1, '\x03'));
		TERM_LOG(L"sendBreakToProcess: sent ^C to PTY");
	}
}

void TerminalPanel::sendTextToTerminal(const std::string& text)
{
	if (!_running || text.empty()) return;
	if (termLogEnabled() && text.find('\x1b') != std::string::npos)
	{
		std::wstring dump;
		dump.reserve(text.size() * 2);
		for (unsigned char c : text)
		{
			if (c == 0x1B) dump += L"\\e";
			else if (c == '\\') dump += L"\\\\";
			else if (c >= 0x20 && c < 0x7F) dump += static_cast<wchar_t>(c);
			else
			{
				wchar_t hex[8] = {};
				_snwprintf(hex, 8, L"\\x%02X", c);
				dump += hex;
			}
		}
		TERM_LOG(L"TX[%lu]: %ls", static_cast<unsigned long>(text.size()), dump.c_str());
	}
	std::lock_guard<std::mutex> inputLock(_inputMutex);
	if (!_running || !_ptyInput) return;

	// WriteFile on a pipe can complete partially when the buffer is full
	// (typical with large pastes) — loop until everything is written.
	const char* p = text.c_str();
	size_t remaining = text.size();
	while (remaining > 0)
	{
		DWORD written = 0;
		if (!::WriteFile(_ptyInput, p, static_cast<DWORD>(remaining), &written, nullptr))
		{
			TERM_LOG(L"sendText: WriteFile failed, err=%lu", ::GetLastError());
			break;
		}
		if (written == 0) break;
		p += written;
		remaining -= written;
	}
}

bool TerminalPanel::copySelectionToClipboard()
{
	std::wstring sel;
	{
		std::lock_guard<std::recursive_mutex> lock(_bufMutex);
		if (!hasSelection()) return false;
		sel = getSelectedText();
	}
	if (sel.empty()) return false;

	HWND hOwner = _hTermWnd ? _hTermWnd : _hSelf;

	// The clipboard can be locked by another process — retry briefly instead
	// of silently failing (a very common cause of "copy doesn't work").
	bool opened = false;
	for (int attempt = 0; attempt < 10 && !opened; ++attempt)
	{
		opened = ::OpenClipboard(hOwner) != FALSE;
		if (!opened) ::Sleep(10);
	}
	if (!opened)
	{
		TERM_LOG(L"copySelection: OpenClipboard failed, err=%lu", ::GetLastError());
		return false;
	}

	::EmptyClipboard();

	const size_t byteSize = (sel.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, byteSize);
	bool ok = false;
	if (hMem)
	{
		void* pMem = ::GlobalLock(hMem);
		if (pMem)
		{
			memcpy(pMem, sel.c_str(), byteSize);
			::GlobalUnlock(hMem);
			// Ownership passes to the clipboard only when SetClipboardData
			// succeeds; otherwise we must free it ourselves (previous code
			// leaked the block on failure).
			if (::SetClipboardData(CF_UNICODETEXT, hMem))
				ok = true;
		}
		if (!ok)
			::GlobalFree(hMem);
	}
	::CloseClipboard();

	if (ok)
	{
		clearSelection();
		if (_hTermWnd) ::InvalidateRect(_hTermWnd, nullptr, FALSE);
	}
	TERM_LOG(L"copySelection: %d chars, ok=%d", static_cast<int>(sel.size()), ok);
	return ok;
}

bool TerminalPanel::pasteFromClipboard()
{
	if (!_running || !_ptyInput) return false;

	HWND hOwner = _hTermWnd ? _hTermWnd : _hSelf;
	if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) &&
	    !::IsClipboardFormatAvailable(CF_TEXT))
		return false;

	bool opened = false;
	for (int attempt = 0; attempt < 10 && !opened; ++attempt)
	{
		opened = ::OpenClipboard(hOwner) != FALSE;
		if (!opened) ::Sleep(10);
	}
	if (!opened)
	{
		TERM_LOG(L"paste: OpenClipboard failed, err=%lu", ::GetLastError());
		return false;
	}

	std::wstring wtext;
	if (HANDLE hData = ::GetClipboardData(CF_UNICODETEXT))
	{
		if (const wchar_t* clipText = static_cast<const wchar_t*>(::GlobalLock(hData)))
		{
			// Never trust the caller's NUL: bound the read by the block size.
			const size_t maxChars = ::GlobalSize(hData) / sizeof(wchar_t);
			size_t n = 0;
			while (n < maxChars && clipText[n] != L'\0') ++n;
			wtext.assign(clipText, n);
			::GlobalUnlock(hData);
		}
	}
	else if (HANDLE hAnsi = ::GetClipboardData(CF_TEXT))
	{
		// Fall back to ANSI text (some apps only publish CF_TEXT).
		if (const char* ansi = static_cast<const char*>(::GlobalLock(hAnsi)))
		{
			const size_t maxBytes = ::GlobalSize(hAnsi);
			size_t ansiLen = 0;
			while (ansiLen < maxBytes && ansi[ansiLen] != '\0') ++ansiLen;
			if (ansiLen <= static_cast<size_t>(INT_MAX))
			{
				int need = ::MultiByteToWideChar(CP_ACP, 0, ansi,
					static_cast<int>(ansiLen), nullptr, 0);
				if (need > 0)
				{
					wtext.resize(static_cast<size_t>(need));
					::MultiByteToWideChar(CP_ACP, 0, ansi,
						static_cast<int>(ansiLen), wtext.data(), need);
				}
			}
			::GlobalUnlock(hAnsi);
		}
	}
	::CloseClipboard();

	if (wtext.empty()) return false;

	// Normalize line endings: a terminal expects CR (0x0D) for "Enter".
	// Pasting raw CRLF makes the shell see two newlines and execute blank
	// lines; pasting LF alone is ignored by some shells. Convert everything
	// to a single CR. Also strip other control chars that would confuse the
	// shell, and drop a trailing newline so a single-line paste doesn't
	// auto-execute unexpectedly.
	std::wstring normalized;
	normalized.reserve(wtext.size());
	for (size_t i = 0; i < wtext.size(); ++i)
	{
		wchar_t c = wtext[i];
		if (c == L'\r')
		{
			normalized += L'\r';
			if (i + 1 < wtext.size() && wtext[i + 1] == L'\n')
				++i;                       // swallow the LF of a CRLF pair
		}
		else if (c == L'\n')
		{
			normalized += L'\r';
		}
		else if (c == L'\t' || c >= 0x20)
		{
			normalized += c;
		}
		// else: drop other C0 controls (e.g. NUL, BEL, ESC) — pasting them
		// can trigger arbitrary terminal/shell behaviour.
	}
	// A trailing CR would immediately submit the command. Keep it only for
	// genuinely multi-line pastes, where the user clearly pasted a script.
	const bool multiline = normalized.find(L'\r') != std::wstring::npos &&
	                       normalized.find(L'\r') != normalized.size() - 1;
	if (!multiline)
	{
		while (!normalized.empty() && normalized.back() == L'\r')
			normalized.pop_back();
	}
	if (normalized.empty()) return false;

	int len = ::WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(),
		static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr);
	if (len <= 0) return false;
	std::string utf8(static_cast<size_t>(len), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(),
		static_cast<int>(normalized.size()), utf8.data(), len, nullptr, nullptr);

	scrollToBottom();

	// Bracketed paste: when the shell has enabled DECSET 2004 (PSReadLine and
	// bash both do), wrap the payload so the shell treats it as literal text
	// instead of interpreting each newline as a command submission.
	if (_bracketedPaste)
		sendTextToTerminal("\x1b[200~" + utf8 + "\x1b[201~");
	else
		sendTextToTerminal(utf8);

	TERM_LOG(L"paste: %d bytes, bracketed=%d", len, _bracketedPaste ? 1 : 0);
	return true;
}

void TerminalPanel::installKbHook()
{
	if (!g_termKbHook)
	{
		g_activeTermPanel = this;
		g_termKbHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, termKbHookProc,
			::GetModuleHandleW(nullptr), 0);
		TERM_LOG(L"installKbHook: hook=%p", g_termKbHook);
	}
}

void TerminalPanel::uninstallKbHook()
{
	if (g_termKbHook)
	{
		::UnhookWindowsHookEx(g_termKbHook);
		g_termKbHook = nullptr;
		g_activeTermPanel = nullptr;
		TERM_LOG(L"uninstallKbHook: removed");
	}
}

// ---- Terminal Display Window ----

TerminalPanel* TerminalPanel::getFromWnd(HWND hwnd)
{
	return reinterpret_cast<TerminalPanel*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK TerminalPanel::termWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	TerminalPanel* pThis = getFromWnd(hwnd);
	if (pThis)
		return pThis->runTermProc(hwnd, message, wParam, lParam);

	if (message == WM_CREATE)
	{
		CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
		pThis = reinterpret_cast<TerminalPanel*>(cs->lpCreateParams);
		::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
		pThis->_hTermWnd = hwnd;
	}
	return ::DefWindowProc(hwnd, message, wParam, lParam);
}

LRESULT TerminalPanel::runTermProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS | DLGC_WANTCHARS;

	case WM_APP_TERM_SYNCSCROLL:
		updateScrollBar();
		return 0;

	case WM_APP_TERM_SELECTALL:
		selectAll();
		return 0;

	case WM_ERASEBKGND:
		// onTermPaint fills the whole client area from the back buffer;
		// letting the system erase first only causes a visible flash.
		return 1;

	case WM_PAINT:
		onTermPaint(hwnd);
		return 0;

	case WM_SIZE:
		onTermResize(hwnd, LOWORD(lParam), HIWORD(lParam));
		return 0;

	case WM_TIMER:
		if (wParam == IDT_TERM_RESIZE)
		{
			::KillTimer(hwnd, IDT_TERM_RESIZE);
			if (_resizePending)
			{
				_resizePending = false;
				TERM_LOG(L"WM_TIMER: debounced resizePseudoConsole %dx%d", _pendingCols, _pendingRows);
				resizePseudoConsole(_pendingCols, _pendingRows);
			}
			return 0;
		}
		break;

	case WM_KEYDOWN:
	{
		// This path is the fallback when the low-level hook is not active
		// (e.g. another app installed a hook ahead of ours). Keep it in sync
		// with termKbHookProc.
		const bool ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

		if ((wParam == 'C' && ctrl && shift) ||
		    (wParam == 'C' && ctrl && !shift && hasSelection()) ||
		    (wParam == VK_INSERT && ctrl))
		{
			copySelectionToClipboard();
			return 0;
		}
		if ((wParam == 'V' && ctrl) || (wParam == VK_INSERT && shift && !ctrl))
		{
			pasteFromClipboard();
			return 0;
		}
		if (wParam == 'A' && ctrl && shift)
		{
			selectAll();
			return 0;
		}
		sendVKeyToTerminal(static_cast<DWORD>(wParam));
		return 0;
	}

	case WM_CHAR:
		// All character generation happens in sendVKeyToTerminal (which is
		// layout-aware). Handling WM_CHAR as well would send every printable
		// key twice.
		return 0;

	case WM_VSCROLL:
	{
		int delta = 0;
		switch (LOWORD(wParam))
		{
		case SB_LINEUP:    delta = -1; break;
		case SB_LINEDOWN:  delta = 1;  break;
		case SB_PAGEUP:    delta = -visibleRows(); break;
		case SB_PAGEDOWN:  delta = visibleRows();  break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION:
		{
			SCROLLINFO si = { sizeof(si), SIF_TRACKPOS };
			::GetScrollInfo(hwnd, SB_VERT, &si);
			delta = si.nTrackPos - static_cast<int>(_scrollTop);
			break;
		}
		}
		scrollTerm(delta);
		return 0;
	}

	case WM_MOUSEWHEEL:
	{
		int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
		UINT linesPerNotch = 3;
		::SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
		if (linesPerNotch == 0 || linesPerNotch > 100) linesPerNotch = 3;
		int lines = static_cast<int>(linesPerNotch) * (-zDelta) / WHEEL_DELTA;
		scrollTerm(lines);
		return 0;
	}

	case WM_SETFOCUS:
		::CreateCaret(hwnd, nullptr, 1, _charHeight);
		::SetCaretPos(_cursorCol * _charWidth, (int)(_cursorRow - _scrollTop) * _charHeight);
		::ShowCaret(hwnd);
		// Install keyboard hook only when terminal has focus
		installKbHook();
		if (_focusReporting && _running)
		{
			TERM_LOG(L"focus: sending focus-in event");
			sendTextToTerminal("\x1b[I");
		}
		return 0;

	case WM_KILLFOCUS:
		::DestroyCaret();
		// Remove keyboard hook so editor can receive keystrokes normally
		uninstallKbHook();
		if (_focusReporting && _running)
		{
			TERM_LOG(L"focus: sending focus-out event");
			sendTextToTerminal("\x1b[O");
		}
		return 0;

	case WM_LBUTTONDOWN:
	{
		::SetFocus(hwnd);
		::SetCapture(hwnd);
		int col; size_t row;
		hitTest(lParam, col, row);
		std::lock_guard<std::recursive_mutex> lock(_bufMutex);
		_selecting = true;
		// Alt+drag = rectangular (column) selection, as in Windows Terminal.
		_selRectangular = (::GetKeyState(VK_MENU) & 0x8000) != 0;
		_selStartCol = col;
		_selStartRow = row;
		_selEndCol = col;
		_selEndRow = row;
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_LBUTTONDBLCLK:
	{
		::SetFocus(hwnd);
		int col; size_t row;
		hitTest(lParam, col, row);
		// Double-click selects a word, triple-click (detected via the shift
		// state trick is unreliable, so we use a timer-free heuristic:
		// Windows only sends DBLCLK, so treat Ctrl+dblclick as line-select)
		if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0)
			selectLineAt(row);
		else
			selectWordAt(col, row);
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_MOUSEMOVE:
	{
		if (_selecting && (wParam & MK_LBUTTON))
		{
			int col; size_t row;
			hitTest(lParam, col, row);
			std::lock_guard<std::recursive_mutex> lock(_bufMutex);
			if (col != _selEndCol || row != _selEndRow)
			{
				_selEndCol = col;
				_selEndRow = row;
				::InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	}

	case WM_LBUTTONUP:
		if (_selecting)
		{
			::ReleaseCapture();
			_selecting = false;
		}
		return 0;

	case WM_RBUTTONUP:
	{
		::SetFocus(hwnd);
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		::ClientToScreen(hwnd, &pt);
		if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0)
		{
			// Shift+right-click always shows the menu.
			showContextMenu(hwnd, pt);
		}
		else if (hasSelection())
		{
			// Console-style: right-click copies the selection...
			copySelectionToClipboard();
		}
		else
		{
			// ...and pastes when there is nothing selected.
			pasteFromClipboard();
		}
		return 0;
	}

	case WM_CONTEXTMENU:
	{
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (pt.x == -1 && pt.y == -1)   // keyboard-invoked (VK_APPS)
		{
			RECT rc; ::GetClientRect(hwnd, &rc);
			pt.x = rc.left; pt.y = rc.top;
			::ClientToScreen(hwnd, &pt);
		}
		showContextMenu(hwnd, pt);
		return 0;
	}

	case WM_MBUTTONUP:
		// X11 convention — middle-click pastes.
		::SetFocus(hwnd);
		pasteFromClipboard();
		return 0;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDM_TERM_COPY:       copySelectionToClipboard(); return 0;
		case IDM_TERM_PASTE:      pasteFromClipboard();       return 0;
		case IDM_TERM_SELECTALL:  selectAll();                return 0;
		case IDM_TERM_CLEAR:
		{
			std::lock_guard<std::recursive_mutex> lock(_bufMutex);
			_buffer.clear();
			_screenTop = 0;
			_scrollTop = 0;
			_cursorRow = 0;
			_cursorCol = 0;
			_autoScroll = true;
			clearSelection();
			ensureRow(0);
			::InvalidateRect(hwnd, nullptr, TRUE);
			updateScrollBar();
			return 0;
		}
		}
		break;

	case WM_COPY:
		copySelectionToClipboard();
		return 0;

	case WM_PASTE:
		pasteFromClipboard();
		return 0;
	}

	return ::DefWindowProc(hwnd, message, wParam, lParam);
}

int TerminalPanel::visibleRows() const
{
	if (_hTermWnd && _charHeight > 0)
	{
		RECT rc;
		::GetClientRect(_hTermWnd, &rc);
		int n = (rc.bottom - rc.top) / _charHeight;
		if (n > 0) return n;
	}
	return _rows > 0 ? _rows : 24;
}

void TerminalPanel::hitTest(LPARAM lParam, int& col, size_t& row) const
{
	// LOWORD/HIWORD on mouse coordinates is wrong for negative values, which
	// happens routinely while dragging a selection outside the window (the
	// old code produced huge bogus columns like 65530 and the selection
	// silently jumped to the end of the line).
	const int x = GET_X_LPARAM(lParam);
	const int y = GET_Y_LPARAM(lParam);

	int c = (_charWidth > 0) ? (x / _charWidth) : 0;
	if (x < 0) c = 0;
	if (c > _cols) c = _cols;
	col = c;

	int screenRow = (_charHeight > 0) ? (y / _charHeight) : 0;
	if (y < 0) screenRow = 0;
	long long r = static_cast<long long>(_scrollTop) + screenRow;
	if (r < 0) r = 0;
	if (!_buffer.empty() && r > static_cast<long long>(_buffer.size()) - 1)
		r = static_cast<long long>(_buffer.size()) - 1;
	row = static_cast<size_t>(r < 0 ? 0 : r);
}

static bool isWordChar(wchar_t c)
{
	return ::iswalnum(c) || c == L'_' || c == L'-' || c == L'.' ||
	       c == L'\\' || c == L'/' || c == L':';
}

void TerminalPanel::selectWordAt(int col, size_t row)
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	if (row >= _buffer.size()) return;
	const auto& cells = _buffer[row].cells;
	if (cells.empty()) return;
	if (col >= static_cast<int>(cells.size())) col = static_cast<int>(cells.size()) - 1;
	if (col < 0) col = 0;

	if (!isWordChar(cells[col].ch)) return;

	int start = col, end = col;
	while (start > 0 && isWordChar(cells[start - 1].ch)) --start;
	while (end + 1 < static_cast<int>(cells.size()) && isWordChar(cells[end + 1].ch)) ++end;

	_selRectangular = false;
	_selStartRow = _selEndRow = row;
	_selStartCol = start;
	_selEndCol = end + 1;   // end is exclusive in getSelectedText's clamping
}

void TerminalPanel::selectLineAt(size_t row)
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	if (row >= _buffer.size()) return;
	_selRectangular = false;
	_selStartRow = _selEndRow = row;
	_selStartCol = 0;
	_selEndCol = static_cast<int>(_buffer[row].cells.size());
}

void TerminalPanel::selectAll()
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	if (_buffer.empty()) return;
	_selRectangular = false;
	_selStartRow = 0;
	_selStartCol = 0;
	_selEndRow = _buffer.size() - 1;
	_selEndCol = static_cast<int>(_buffer.back().cells.size());
	if (_hTermWnd) ::InvalidateRect(_hTermWnd, nullptr, FALSE);
}

void TerminalPanel::showContextMenu(HWND hwnd, POINT ptScreen)
{
	HMENU hMenu = ::CreatePopupMenu();
	if (!hMenu) return;

	const bool canCopy = hasSelection();
	const bool canPaste = ::IsClipboardFormatAvailable(CF_UNICODETEXT) ||
	                      ::IsClipboardFormatAvailable(CF_TEXT);

	::AppendMenuW(hMenu, MF_STRING | (canCopy ? MF_ENABLED : MF_GRAYED),
		IDM_TERM_COPY, L"Copy\tCtrl+Shift+C");
	::AppendMenuW(hMenu, MF_STRING | (canPaste ? MF_ENABLED : MF_GRAYED),
		IDM_TERM_PASTE, L"Paste\tCtrl+V");
	::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(hMenu, MF_STRING, IDM_TERM_SELECTALL, L"Select All\tCtrl+Shift+A");
	::AppendMenuW(hMenu, MF_STRING, IDM_TERM_CLEAR, L"Clear Terminal");

	// The keyboard hook must be off while a modal menu loop is running,
	// otherwise the menu never sees the arrow keys / Enter it needs.
	uninstallKbHook();
	UINT cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
		ptScreen.x, ptScreen.y, 0, hwnd, nullptr);
	::DestroyMenu(hMenu);
	if (::GetFocus() == hwnd)
		installKbHook();

	if (cmd != 0)
		::SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

bool TerminalPanel::hasSelection() const
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	return _selStartCol >= 0 && _selEndCol >= 0 &&
	       (_selStartRow != _selEndRow || _selStartCol != _selEndCol);
}

void TerminalPanel::clearSelection()
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	_selStartCol = -1;
	_selEndCol = -1;
	_selecting = false;
	_selRectangular = false;
}

std::wstring TerminalPanel::getSelectedText() const
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	if (!hasSelection()) return L"";

	int sCol1, sCol2;
	size_t sRow1, sRow2;
	if (_selStartRow < _selEndRow || (_selStartRow == _selEndRow && _selStartCol <= _selEndCol))
	{
		sRow1 = _selStartRow; sCol1 = _selStartCol;
		sRow2 = _selEndRow;   sCol2 = _selEndCol;
	}
	else
	{
		sRow1 = _selEndRow;   sCol1 = _selEndCol;
		sRow2 = _selStartRow; sCol2 = _selStartCol;
	}

	// Rectangular (Alt+drag) selection: the same column range on every row.
	if (_selRectangular)
	{
		int c1 = (std::min)(_selStartCol, _selEndCol);
		int c2 = (std::max)(_selStartCol, _selEndCol);
		sCol1 = c1;
		sCol2 = c2;
	}

	std::wstring result;
	for (size_t r = sRow1; r <= sRow2 && r < _buffer.size(); r++)
	{
		const Line& line = _buffer[r];
		const int lineLen = static_cast<int>(line.cells.size());

		int start, end;
		if (_selRectangular)
		{
			start = sCol1;
			end = sCol2;
		}
		else
		{
			start = (r == sRow1) ? sCol1 : 0;
			// The end column is exclusive; the previous code used an inclusive
			// end which dropped the last selected character.
			end = (r == sRow2) ? sCol2 : lineLen;
		}
		if (start < 0) start = 0;
		if (end > lineLen) end = lineLen;

		std::wstring rowText;
		for (int c = start; c < end; c++)
			rowText += line.cells[c].ch;

		// Trim trailing spaces (cells are space-filled to the right margin)
		while (!rowText.empty() && rowText.back() == L' ')
			rowText.pop_back();

		if (r > sRow1) result += L"\r\n";
		result += rowText;
	}
	return result;
}

void TerminalPanel::onTermPaint(HWND hwnd)
{
	PAINTSTRUCT ps;
	HDC hdc = ::BeginPaint(hwnd, &ps);

	RECT rc;
	::GetClientRect(hwnd, &rc);
	const int clientW = rc.right - rc.left;
	const int clientH = rc.bottom - rc.top;
	if (clientW <= 0 || clientH <= 0)
	{
		::EndPaint(hwnd, &ps);
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(_bufMutex);

	const int rows = (_charHeight > 0) ? (clientH + _charHeight - 1) / _charHeight : 0;

	// Double-buffer the *entire* client area. Previously only cols*charWidth
	// was blitted, leaving an unpainted (white) strip on the right and bottom.
	HDC memDC = ::CreateCompatibleDC(hdc);
	HBITMAP memBmp = ::CreateCompatibleBitmap(hdc, clientW, clientH);
	HBITMAP oldBmp = static_cast<HBITMAP>(::SelectObject(memDC, memBmp));
	HFONT oldFont = static_cast<HFONT>(::SelectObject(memDC, _hFont));
	::SetBkMode(memDC, OPAQUE);

	// Background
	{
		RECT all = { 0, 0, clientW, clientH };
		HBRUSH bg = ::CreateSolidBrush(_stdColors[0]);
		::FillRect(memDC, &all, bg);
		::DeleteObject(bg);
	}

	// Normalized selection bounds
	bool selOn = hasSelection();
	size_t sRow1 = 0, sRow2 = 0;
	int sCol1 = 0, sCol2 = 0;
	if (selOn)
	{
		if (_selStartRow < _selEndRow || (_selStartRow == _selEndRow && _selStartCol <= _selEndCol))
		{
			sRow1 = _selStartRow; sCol1 = _selStartCol;
			sRow2 = _selEndRow;   sCol2 = _selEndCol;
		}
		else
		{
			sRow1 = _selEndRow;   sCol1 = _selEndCol;
			sRow2 = _selStartRow; sCol2 = _selStartCol;
		}
		if (_selRectangular)
		{
			int c1 = (std::min)(_selStartCol, _selEndCol);
			int c2 = (std::max)(_selStartCol, _selEndCol);
			sCol1 = c1; sCol2 = c2;
		}
	}

	// Draw visible lines, batching runs of cells that share fg/bg into a
	// single ExtTextOutW call. Drawing every cell individually (the old code)
	// meant ~2000 GDI brush create/destroy pairs per repaint, which made the
	// terminal visibly lag while output streamed in.
	std::wstring run;
	for (int screenRow = 0; screenRow < rows; screenRow++)
	{
		size_t bufferRow = _scrollTop + static_cast<size_t>(screenRow);
		if (bufferRow >= _buffer.size()) break;

		const Line& line = _buffer[bufferRow];
		const int y = screenRow * _charHeight;
		const int lineLen = static_cast<int>(line.cells.size());

		int col = 0;
		while (col < lineLen)
		{
			// Is this cell inside the selection?
			auto inSel = [&](int c) -> bool {
				if (!selOn || bufferRow < sRow1 || bufferRow > sRow2) return false;
				if (_selRectangular) return c >= sCol1 && c < sCol2;
				const int lo = (bufferRow == sRow1) ? sCol1 : 0;
				const int hi = (bufferRow == sRow2) ? sCol2 : lineLen;
				return c >= lo && c < hi;
			};

			const Cell& first = line.cells[col];
			const bool selFirst = inSel(col);
			COLORREF fg = first.fg, bg = first.bg;
			if (selFirst) { std::swap(fg, bg); }

			int runEnd = col;
			run.clear();
			while (runEnd < lineLen)
			{
				const Cell& cell = line.cells[runEnd];
				if (cell.fg != first.fg || cell.bg != first.bg || inSel(runEnd) != selFirst)
					break;
				run += cell.ch ? cell.ch : L' ';
				++runEnd;
			}

			const int x = col * _charWidth;
			const int w = (runEnd - col) * _charWidth;
			RECT runRect = { x, y, x + w, y + _charHeight };
			::SetTextColor(memDC, fg);
			::SetBkColor(memDC, bg);
			::ExtTextOutW(memDC, x, y, ETO_CLIPPED | ETO_OPAQUE, &runRect,
				run.c_str(), static_cast<UINT>(run.size()), nullptr);

			col = runEnd;
		}
	}

	// Cursor — drawn into the back buffer so it can't flicker.
	if (_cursorVisible && ::GetFocus() == hwnd &&
	    _cursorRow >= _scrollTop && _cursorRow < _scrollTop + static_cast<size_t>(rows))
	{
		const int cursorScreenRow = static_cast<int>(_cursorRow - _scrollTop);
		const int cx = _cursorCol * _charWidth;
		const int cy = cursorScreenRow * _charHeight;
		RECT cursorRect = { cx, cy, cx + _charWidth, cy + _charHeight };
		::InvertRect(memDC, &cursorRect);
	}

	::BitBlt(hdc, 0, 0, clientW, clientH, memDC, 0, 0, SRCCOPY);

	::SelectObject(memDC, oldFont);
	::SelectObject(memDC, oldBmp);
	::DeleteObject(memBmp);
	::DeleteDC(memDC);

	::EndPaint(hwnd, &ps);
}

void TerminalPanel::onTermResize(HWND hwnd, int width, int height)
{
	TERM_LOG(L"onTermResize: %dx%d", width, height);
	if (!_hFont) return;
	if (width <= 0 || height <= 0) return;
	if (_charWidth <= 0 || _charHeight <= 0) return;

	int newCols = width / _charWidth;
	int newRows = height / _charHeight;

	if (newCols < 20) newCols = 20;
	if (newRows < 5)  newRows = 5;

	if (newCols != _cols || newRows != _rows)
	{
		std::lock_guard<std::recursive_mutex> lock(_bufMutex);
		_cols = static_cast<SHORT>(newCols);
		_rows = static_cast<SHORT>(newRows);

		// Grow lines to the new width; never shrink, so text that was already
		// printed beyond the new margin is preserved when the panel is widened
		// again (shrinking here used to permanently truncate scrollback).
		for (auto& line : _buffer)
		{
			if (line.cells.size() < static_cast<size_t>(_cols))
				line.cells.resize(_cols, { L' ', _stdColors[7], _stdColors[0] });
		}

		// Don't push every intermediate size straight to ConPTY: the panel
		// fires a burst of WM_SIZE while docking/layout settles, and each
		// ResizePseudoConsole call makes PowerShell/PSReadLine repaint its
		// prompt. If a new resize interrupts that repaint before it finishes,
		// the shell can be left with a cleared screen and nothing redrawn --
		// i.e. a terminal that looks permanently blank. Debounce: remember
		// the latest size and only tell the PTY once sizing has been quiet
		// for RESIZE_DEBOUNCE_MS.
		_resizePending = true;
		_pendingCols = _cols;
		_pendingRows = _rows;
		::SetTimer(hwnd, IDT_TERM_RESIZE, RESIZE_DEBOUNCE_MS, nullptr);

		updateScrollBar();
		if (_autoScroll) scrollToBottom();

		::InvalidateRect(hwnd, nullptr, TRUE);
	}
	else
	{
		updateScrollBar();
	}
}


void TerminalPanel::scrollTerm(int delta)
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	if (_buffer.empty() || delta == 0) return;

	const int rows = visibleRows();
	int maxScroll = static_cast<int>(_buffer.size()) - rows;
	if (maxScroll < 0) maxScroll = 0;

	int newTop = static_cast<int>(_scrollTop) + delta;
	if (newTop < 0) newTop = 0;
	if (newTop > maxScroll) newTop = maxScroll;

	if (newTop == static_cast<int>(_scrollTop)) return;
	_scrollTop = static_cast<size_t>(newTop);
	// Scrolling up detaches from the live tail; scrolling back to the bottom
	// re-attaches, so new output follows again.
	_autoScroll = (newTop >= maxScroll);

	updateScrollBar();
	if (_hTermWnd)
		::InvalidateRect(_hTermWnd, nullptr, FALSE);
}

void TerminalPanel::scrollToBottom()
{
	std::lock_guard<std::recursive_mutex> lock(_bufMutex);
	const int rows = visibleRows();
	size_t maxTop = (_buffer.size() > static_cast<size_t>(rows))
		? _buffer.size() - rows : 0;
	_autoScroll = true;
	if (_scrollTop == maxTop) return;
	_scrollTop = maxTop;
	updateScrollBar();
	if (_hTermWnd)
		::InvalidateRect(_hTermWnd, nullptr, FALSE);
}

void TerminalPanel::updateScrollBar()
{
	if (!_hTermWnd) return;

	// The window was created with WS_VSCROLL but the scroll info was never
	// set, so the scrollbar was a permanently disabled decoration.
	const int rows = visibleRows();
	SCROLLINFO si = {};
	si.cbSize = sizeof(si);
	si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
	si.nMin = 0;
	si.nMax = static_cast<int>(_buffer.size()) > 0 ? static_cast<int>(_buffer.size()) - 1 : 0;
	si.nPage = static_cast<UINT>(rows);
	si.nPos = static_cast<int>(_scrollTop);
	::SetScrollInfo(_hTermWnd, SB_VERT, &si, TRUE);
}

// ---- Docking Panel Interface ----

intptr_t CALLBACK TerminalPanel::run_dlgProc(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
	{
		TERM_LOG(L"WM_INITDIALOG: creating terminal window...");
		// Create the terminal display window
		WNDCLASSW wc = {};
		wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
		wc.lpfnWndProc = termWndProc;
		wc.hInstance = _hInst;
		wc.hCursor = ::LoadCursor(nullptr, IDC_IBEAM);
		wc.hbrBackground = (HBRUSH)::GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = L"NppTerminalWindow";

		ATOM atom = ::RegisterClassW(&wc);
		if (!atom && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return FALSE;

		RECT rc;
		::GetClientRect(_hSelf, &rc);

		_hTermWnd = ::CreateWindowW(
			L"NppTerminalWindow", nullptr,
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
			0, 0, rc.right, rc.bottom,
			_hSelf, nullptr, _hInst, this
		);

		if (!_hTermWnd) { TERM_LOG(L"WM_INITDIALOG: CreateWindow failed!"); return FALSE; }
		TERM_LOG(L"WM_INITDIALOG: terminal window created, hwnd=%p", _hTermWnd);

		// Create font (DPI-aware, 12pt Consolas)
		HDC hdc = ::GetDC(_hTermWnd);
		int fontHeight = -::MulDiv(12, ::GetDeviceCaps(hdc, LOGPIXELSY), 72);
		_hFont = ::CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, FIXED_PITCH | FF_DONTCARE, L"Consolas");

		// Measure character size
		::SelectObject(hdc, _hFont);
		TEXTMETRICW tm = {};
		::GetTextMetricsW(hdc, &tm);
		_charWidth = tm.tmAveCharWidth;
		_charHeight = tm.tmHeight;
		::ReleaseDC(_hTermWnd, hdc);

		_cols = rc.right / _charWidth;
		_rows = rc.bottom / _charHeight;
		if (_cols < 20) _cols = 80;
		if (_rows < 5)  _rows = 24;

		return TRUE;
	}

	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	{
		// Return dark background brush for the dialog and its controls
		static HBRUSH hDarkBg = ::CreateSolidBrush(RGB(12, 12, 12));
		::SetBkColor((HDC)wParam, RGB(12, 12, 12));
		::SetTextColor((HDC)wParam, RGB(220, 220, 220));
		return (INT_PTR)hDarkBg;
	}

	case WM_SIZE:
	{
		RECT rc;
		::GetClientRect(_hSelf, &rc);
		if (_hTermWnd)
			::SetWindowPos(_hTermWnd, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER);
		return TRUE;
	}

	case WM_SETFOCUS:
		TERM_LOG(L"run_dlgProc: WM_SETFOCUS");
		if (_hTermWnd)
			::SetFocus(_hTermWnd);
		return TRUE;

	case WM_KEYDOWN:
	case WM_CHAR:
	case WM_KEYUP:
		// Do NOT forward here. The terminal child window (and the low-level
		// hook) already handle every keystroke; forwarding from the dialog too
		// caused each character to be transmitted twice.
		return TRUE;

	case WM_NOTIFY:
	{
		auto* pnmh = reinterpret_cast<LPNMHDR>(lParam);
		if (pnmh->hwndFrom == _hParent && pnmh->code == DMN_CLOSE)
		{
			TERM_LOG(L"run_dlgProc: DMN_CLOSE received");
			terminate();
			if (_hTermWnd)
			{
				::DestroyWindow(_hTermWnd);
				_hTermWnd = nullptr;
			}
			_isClosed = true;
			return TRUE;
		}
		break;
	}

	case WM_DESTROY:
		TERM_LOG(L"run_dlgProc: WM_DESTROY");
		terminate();
		if (_hTermWnd)
		{
			::DestroyWindow(_hTermWnd);
			_hTermWnd = nullptr;
		}
		return TRUE;
	}

	return FALSE;
}

void TerminalPanel::destroy()
{
	terminate();
	if (_hTermWnd)
	{
		::DestroyWindow(_hTermWnd);
		_hTermWnd = nullptr;
	}
	StaticDialog::destroy();
}
