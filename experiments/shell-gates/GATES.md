# Shell architecture: Gates 1 and 3

Status: **IN PROGRESS** (2026-11-09). This file is kept current; the "State" line of each section says where it stands.

The shell architecture: the 32-bit EXE keeps window, 2D UI (Lua VM + Controls), audio, netcode, save/load and asset
loading, with its world-rendering subsystems switched off (`[DisableSystems]` in config.ini); a 64-bit server owns
game state and renders the world into a shared D3D11 texture that the EXE composites under its UI.
Feasibility study: https://claude.ai/artifact/Ebo4vHqPXFU9tKpisduAoG.

Branch: `claude/shell-gates-d8e422`, fast-forwarded to `f181104a2` (memory-diagnostics = master + the DLL that is
installed in the game: Lua channel, MemoryImports, EngineQueueGuard), so experiment DLLs match the installed mods.

## Verdicts

| Gate | Verdict | One line |
|---|---|---|
| 1. The shell survives without its world | pending | waiting for the probe results |
| 3. The server can draw into the shell | pending | prototype under construction |

## Gate 1: the shell survives without its world

State: waiting for the parent session's probes (each DisableSystems key alone, then two combined configurations).

### Probe results (forwarded)

### Re-run of the decisive configuration (instrumented)

Instrumentation: `tools/addrspace.py` (read-only: private bytes, working set, committed / reserved / free, largest
free block, free blocks >= 1 / 16 MB, free below 2 GB; one-shot or CSV every N s).

### Owners of the 118.75 MB (0x26) and 59.38 MB (0x25) blocks

### Memory left in the shell, and what still works

## Gate 3: the server can draw into the shell

State: prototype (x64 server, x86 compositor DLL, x86 stand-in host) being built in `compositor/`.

### Prototype

### Measurements

### Camera ownership (static analysis)

## Running experiments without stealing Art's focus

State: options being evaluated against a stand-in D3D11 app in `nofocus/`.

## Tooling

State: **installed** (Art approved 2026-11-09 the portable installs under `C:\Users\Art\tools`). Full table with
sources, SHA-256 and verification in `tooling/TOOLING.md`.

- Temurin JDK 21.0.13+9; **Ghidra 12.1.4** (pinned to match ghidra-mcp 7.0.0). Project
  `C:\Users\Art\tools\ghidra-projects\civ5\civ5.gpr`: DX11 EXE (timestamp 0x546CD0A8) auto-analysed, 21,009
  functions; the 10 addresses of PR #13379's `CvExeAddresses.h` applied as labels (all 6 signatures match).
- **ghidra-mcp 7.0.0**: extension in `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions`, bridge venv in
  `C:\Users\Art\tools\ghidra-mcp-7.0.0\venv`, project `.mcp.json` at the worktree root. Bridge verified over stdio;
  Ghidra side not yet (needs Art to accept Ghidra's first-start User Agreement). To enable: run
  `tooling\ghidra.cmd`, accept the agreement, enable GhidraMCPPlugin (File > Configure > Utility), open `civ5.gpr`
  and the EXE (do not re-analyse), check with `tooling\ghidra_mcp_check.cmd`, then approve the `ghidra-mcp` server
  when Claude Code starts in this worktree.
- RenderDoc 1.47 portable (x86 capture layer present); Sysinternals VMMap 3.4 (verified on a 32-bit process),
  Process Explorer 17.21, Process Monitor 4.13 (needs elevation, not run); MinHook v1.3.4 source.
- 64-bit and 32-bit D3D11 builds with clang-cl 19.1.7 + VS 2022 libs + SDK 10.0.26100.0 verified
  (`tooling/d3d11_hello`), RTX 4080 at FL 11_0.
- **The Civ 5 Linux binary is not on the machine** (no Linux depot in either Steam library, no ELF files).

Present on the machine: VS 2022 Community (`C:\Program Files\Microsoft Visual Studio\2022\Community`, vcvarsall),
Windows SDK 10.0.22621.0 and 10.0.26100.0, LLVM clang-cl (`C:\Program Files\LLVM\bin`, -m32 and -m64 verified for
a trivial program), Python 3.11 (anaconda).
