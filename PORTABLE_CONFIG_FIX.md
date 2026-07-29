# Portable Configuration Fix

## Problem

The portable ZIP did not create `config.xml` and `session.xml` beside the executable after extraction.

## Cause

The package was missing `doLocalConf.xml`. This marker file tells NodePlus-CODE to save configuration beside the executable instead of `%APPDATA%/Notepad++/`.

## Fix

Add `doLocalConf.xml` beside `nodeplus-code.exe`. Its presence is what matters.

The portable package should exclude machine-specific `config.xml`, `session.xml`, log, and backup files.

## Expected Behavior

After normal launch and exit, `config.xml` stores UI and Folder as Workspace settings, while `session.xml` stores open files and tabs when remembering the last session is enabled. Force-terminating the process can prevent saving.

## Release Update

The corrected ZIP was rebuilt with `doLocalConf.xml` and uploaded to the `v0.1.0` GitHub release.
