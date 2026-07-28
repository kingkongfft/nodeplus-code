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


#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include "DockingDlgInterface.h"
#include "TerminalPanel_rc.h"

// ConPTY function types (dynamically loaded from kernel32)
typedef HRESULT (WINAPI *PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *PFN_ClosePseudoConsole)(HPCON);

#define TERM_PANELTITLE         L"Terminal"
#define TERM_NODE_NAME          "Terminal"

class TerminalPanel : public DockingDlgInterface
{
public:
	TerminalPanel() : DockingDlgInterface(IDD_TERMINAL_PANEL)
	{
		// Always use classic terminal colors (dark background) regardless of
		// Notepad++ theme. Real terminals (cmd, PowerShell, VS Code integrated
		// terminal) are always dark — a light terminal is confusing and hard to
		// read, especially for ANSI-colored output.
		_stdColors[0]  = RGB(12,  12,  12);    // 0  Background (near-black)
		_stdColors[1]  = RGB(255, 85,  85);    // 1  Red
		_stdColors[2]  = RGB(85,  255, 85);    // 2  Green
		_stdColors[3]  = RGB(255, 255, 85);    // 3  Yellow
		_stdColors[4]  = RGB(85,  85,  255);   // 4  Blue
		_stdColors[5]  = RGB(255, 85,  255);   // 5  Magenta
		_stdColors[6]  = RGB(85,  255, 255);   // 6  Cyan
		_stdColors[7]  = RGB(220, 220, 220);   // 7  Foreground (light gray)
		_stdColors[8]  = RGB(100, 100, 100);   // 8  Bright Black (gray)
		_stdColors[9]  = RGB(255, 120, 120);   // 9  Bright Red
		_stdColors[10] = RGB(120, 255, 120);   // 10 Bright Green
		_stdColors[11] = RGB(255, 255, 120);   // 11 Bright Yellow
		_stdColors[12] = RGB(120, 120, 255);   // 12 Bright Blue
		_stdColors[13] = RGB(255, 120, 255);   // 13 Bright Magenta
		_stdColors[14] = RGB(120, 255, 255);   // 14 Bright Cyan
		_stdColors[15] = RGB(255, 255, 255);   // 15 Bright White

		_currentFg = _stdColors[7];
		_currentBg = _stdColors[0];
	}
	~TerminalPanel();

	// Launch a shell in the terminal
	void launchShell(const std::wstring& shellPath, const std::wstring& workingDir);
	bool isRunning() const { return _running; }
	HWND getTerminalHwnd() const { return _hTermWnd; }
	void terminate();

	// Keyboard input (public for hook)
	void sendKeyToTerminal(WPARAM wParam, LPARAM lParam);
	void sendVKeyToTerminal(DWORD vkCode);
	void sendTextToTerminal(const std::string& text);
	void installKbHook();
	void uninstallKbHook();

	// Clipboard (public for hook access)
	bool copySelectionToClipboard();
	bool pasteFromClipboard();

	// Text selection (public for hook access)
	bool hasSelection() const;
	void clearSelection();
	void selectAll();

	// Keyboard support - Ctrl+Break/Break sends Ctrl+C to the process
	void sendBreakToProcess();

protected:
	intptr_t CALLBACK run_dlgProc(UINT message, WPARAM wParam, LPARAM lParam) override;
	void destroy() override;

private:
	// ConPTY
	bool initConPty();
	bool createPseudoConsole(COORD size);
	bool startProcess(const std::wstring& cmdLine, const std::wstring& workingDir);
	void resizePseudoConsole(SHORT cols, SHORT rows);

	// Reader thread
	void readerThreadProc();
	void processOutput(const char* data, DWORD len);
	void appendText(const std::wstring& text, COLORREF fg, COLORREF bg);
	void handleANSIEscape();

	// Terminal display window
	static LRESULT CALLBACK termWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	static TerminalPanel* getFromWnd(HWND hwnd);
	LRESULT runTermProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	void onTermPaint(HWND hwnd);
	void onTermResize(HWND hwnd, int width, int height);
	void scrollTerm(int delta);
	void scrollToBottom();
	void updateScrollBar();
	int  visibleRows() const;
	void showContextMenu(HWND hwnd, POINT ptScreen);
	void hitTest(LPARAM lParam, int& col, size_t& row) const;
	void selectWordAt(int col, size_t row);
	void selectLineAt(size_t row);

	// ANSI state
	void resetANSIParser();
	void applySGR(int param);
	void processSGR();
	void putChar(wchar_t wc);
	void newLine();
	void ensureRow(size_t row);

	// ConPTY handles
	HPCON _hPC = nullptr;
	HANDLE _ptyInput = nullptr;   // write to this -> goes to process stdin
	HANDLE _ptyOutput = nullptr;  // read from this -> process stdout
	HANDLE _hProcess = nullptr;
	HANDLE _hThread = nullptr;
	std::thread _readerThread;
	std::atomic<bool> _running{false};
	std::mutex _inputMutex;

	// Dynamically loaded ConPTY functions
	PFN_CreatePseudoConsole  _pfnCreatePseudoConsole = nullptr;
	PFN_ResizePseudoConsole  _pfnResizePseudoConsole = nullptr;
	PFN_ClosePseudoConsole   _pfnClosePseudoConsole = nullptr;

	// Terminal display
	HWND _hTermWnd = nullptr;
	HFONT _hFont = nullptr;
	int _charWidth = 8;
	int _charHeight = 16;
	SHORT _cols = 80;
	SHORT _rows = 24;

	// Text buffer
	struct Cell {
		wchar_t ch;
		COLORREF fg;
		COLORREF bg;
	};
	struct Line {
		std::vector<Cell> cells;
	};
	std::deque<Line> _buffer;
	mutable std::recursive_mutex _bufMutex;   // guards _buffer / cursor / selection
	size_t _scrollTop = 0;    // first visible line index
	size_t _maxLines = 10000;     // max buffer lines
	bool _autoScroll = true;  // stick to bottom unless user scrolled up

	// Cursor
	int _cursorCol = 0;
	size_t _cursorRow = 0;    // row in buffer (not screen-relative)
	bool _cursorVisible = true;
	size_t _screenTop = 0;    // buffer row that maps to screen row 0 (for CUP)

	// Text selection
	bool _selecting = false;
	int _selStartCol = -1;
	size_t _selStartRow = 0;
	int _selEndCol = -1;
	size_t _selEndRow = 0;
	bool _selRectangular = false;
	std::wstring getSelectedText() const;

	// ANSI parser state
	enum AnsiState { TS_NORMAL, TS_ESCAPE, TS_CSI, TS_OSC, TS_CHARSET };
	AnsiState _ansiState = TS_NORMAL;
	std::string _ansiParams;
	char _ansiFinalByte = 0;
	bool _ansiQuestionMark = false;
	bool _ansiOscEsc = false;

	// UTF-8 incremental decoder (multi-byte sequences may span ReadFile chunks)
	unsigned int _utf8Acc = 0;
	int _utf8Remaining = 0;

	// Terminal modes
	std::atomic<bool> _bracketedPaste{false};   // DECSET 2004 — shells like PSReadLine enable this
	std::atomic<bool> _appCursorKeys{false};    // DECCKM — SS3 vs CSI arrow keys

	// SGR state
	COLORREF _currentFg = RGB(192, 192, 192);  // white
	COLORREF _currentBg = RGB(0, 0, 0);        // black
	bool _bold = false;
	bool _underline = false;
	bool _inverse = false;

	// Standard terminal colors (16-color palette)
	COLORREF _stdColors[16];
};
