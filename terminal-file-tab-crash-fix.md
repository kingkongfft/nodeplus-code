# File Tab Crash with Terminal Tabs - Fixed

**Date**: 2026-07-29
**Status**: Fixed

With embedded PowerShell tabs open, closing an ordinary file tab caused NodePlus-CODE to exit. Terminal tabs are not ordinary Buffer objects; task-list/MRU generation could produce an invalid BufferID and dereference a null Buffer pointer. The close path could also route a terminal replacement tab through ordinary file-buffer activation.

The fix skips terminal tabs and invalid buffers in task-list/MRU generation, uses terminal-specific activation, adds defensive checks, and removes terminal objects after their tab entries are removed.

Files changed: PowerEditor/src/Notepad_plus.cpp, PowerEditor/src/NppNotification.cpp, and PowerEditor/src/NppIO.cpp.

Validation: reproduced with multiple PowerShell and file tabs, then verified fixed after ./build-local.sh completed successfully.
