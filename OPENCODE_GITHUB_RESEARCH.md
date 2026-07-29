# OpenCode Embedded Terminal Research

## Finding

OpenCode launches successfully inside the embedded PowerShell terminal. The process remains responsive and the terminal answers the major OpenTUI capability probes. The remaining failure is incomplete terminal emulation and rendering, not PowerShell discovery or ConPTY startup.

The current terminal uses a hand-written VT/ANSI parser. OpenTUI produces full-screen frames and relies on terminal behavior broader than the current implementation. The log shows OpenCode entering the alternate screen and sending a large blank redraw frame, but no usable interface appears in the custom renderer.

## GitHub Research

- microsoft/terminal samples/ConPTY/EchoCon demonstrates pipes, CreatePseudoConsole, process spawning, and output reading. It does not implement a terminal emulator or VT renderer.
- Microsoft in-process ConPTY design documents the required architecture: ConPTY -> VT parser -> terminal buffer -> renderer. A pipe reader plus a partial escape-sequence parser is not equivalent to Windows Terminal.
- vinsworldcom/nppConsole embeds a native console for cmd.exe and PowerShell. It is useful for docking and console hosting, but does not provide a modern OpenTUI-compatible VT renderer.
- remorses/ghostty-opentui uses Ghostty's mature terminal parser for ANSI and terminal output, demonstrating the more reliable implementation direction.

## Recommendation

The current implementation is suitable as a basic embedded shell console, but not as a fully compatible terminal emulator for OpenCode. The durable fix is to replace or substantially extend the hand-written parser with a mature VT implementation, such as a reusable Ghostty terminal parser or another complete terminal control.

Adding individual replies such as OSC 99, OSC 1337, XTVERSION, focus events, and cursor reports may improve compatibility, but cannot guarantee correct behavior because OpenTUI depends on many interacting terminal states and rendering semantics.

## References

- microsoft/terminal ConPTY sample: samples/ConPTY/EchoCon
- Microsoft in-process ConPTY design: doc/specs/#13000 - In-process ConPTY.md
- vinsworldcom/nppConsole
- remorses/ghostty-opentui
