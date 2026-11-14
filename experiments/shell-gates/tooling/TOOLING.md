# Shell-gates tooling

Installed 2026-11-09. All tools are portable, user-level installs under `C:\Users\Art\tools`: no admin, no
system settings, no PATH changes. Downloads are kept in `C:\Users\Art\tools\_downloads\` (852 MB; safe to delete).
Launchers in this folder hard-code the paths below.

Files here: `ghidra.cmd`, `ghidra_import_civ5.cmd`, `ghidra_apply_pr13379.cmd`, `ghidra_mcp_check.cmd`,
`ghidra_scripts\ApplyPr13379Symbols.java`, `pr13379_symbols.csv`, `vmmap.cmd`, `procexp.cmd`, `procmon.cmd`,
`renderdoc.cmd`, `d3d11_hello\` (source + build script; `build\` is output). Plus `.mcp.json` at the worktree root.

Git note: the repo's `.gitignore` starts with `*`, so this folder's files and the worktree's `.mcp.json` need
`git add -f`.

| Tool | Version | Install path |
|---|---|---|
| Eclipse Temurin JDK | 21.0.13+9 | `C:\Users\Art\tools\temurin-jdk-21.0.13+9` |
| Ghidra | 12.1.4 | `C:\Users\Art\tools\ghidra-12.1.4` |
| ghidra-mcp (bethington) | 7.0.0 | extension: `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions\GhidraMCP`; bridge: `C:\Users\Art\tools\ghidra-mcp-7.0.0\venv` |
| Ghidra project `civ5` | - | `C:\Users\Art\tools\ghidra-projects\civ5\civ5.gpr` |
| VMMap | 3.4 | `C:\Users\Art\tools\vmmap-3.4` |
| Process Monitor | 4.13 | `C:\Users\Art\tools\procmon-4.13` |
| Process Explorer | 17.21 | `C:\Users\Art\tools\procexp-17.21` |
| RenderDoc | 1.47 (stable, portable 64-bit) | `C:\Users\Art\tools\renderdoc-1.47` |
| MinHook | v1.3.4 (source) | `C:\Users\Art\tools\minhook-v1.3.4` |

## Temurin JDK 21

- Source: https://github.com/adoptium/temurin21-binaries/releases/tag/jdk-21.0.13%2B9,
  asset `OpenJDK21U-jdk_x64_windows_hotspot_21.0.13_9.zip`.
- SHA-256 `a4dbaa05fca4bd89d84584e58c459bd732efebb1722af80ab13c6516832004f9`, matches the publisher's
  `.sha256.txt`.
- Not on PATH and JAVA_HOME is not set globally; Ghidra is pointed at it (below).
- Verified: `C:\Users\Art\tools\temurin-jdk-21.0.13+9\bin\java -version` prints
  `OpenJDK Runtime Environment Temurin-21.0.13+9 (build 21.0.13+9-LTS)`.

## Ghidra

- Source: https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.4_build,
  asset `ghidra_12.1.4_PUBLIC_20260921.zip`.
- SHA-256 `ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db`, matches the hash in the release notes.
- Why 12.1.4 and not the newer 12.2 (2026-10-22): ghidra-mcp 7.0.0 is built for 12.1.4 and a Ghidra extension
  loads only in the exact Ghidra version it was built for.
- JDK: `support\launch.properties` has `JAVA_HOME_OVERRIDE=C:/Users/Art/tools/temurin-jdk-21.0.13+9`; the
  launchers also set `JAVA_HOME`.
- Launch: `ghidra.cmd` (GUI). Headless: `ghidra_import_civ5.cmd`, `ghidra_apply_pr13379.cmd`.
- First GUI start shows Ghidra's User Agreement; Art accepts it himself (it was not accepted for him).

### Project civ5: the DX11 EXE

- Input: a copy of `CivilizationV_DX11.exe` at `C:\Users\Art\tools\ghidra-projects\bin\CivilizationV_DX11.exe`,
  SHA-256 `b2ce96aefcf27d1ed6d5d021c5ad1e4fdb0fd01628e115da880596f00670c88e` (identical to the installed one;
  Steam build id 4390913; PE TimeDateStamp `0x546CD0A8`, the DX11 build that PR 13379 keys on).
- Import + auto-analysis: `ghidra_import_civ5.cmd` (analyzeHeadless, 8 GB heap). Ran 2026-11-09 17:20-17:26,
  analysis 353 s, "Analysis succeeded / Save succeeded / Import succeeded". Logs in
  `C:\Users\Art\tools\ghidra-projects\logs\`.
- Result: x86:LE:32:default, compiler spec `windows`, image base `0x00400000`, 21,009 functions, 78,790 symbols.
  Sections: `.text` 0x401000 (5.8 MB), `.rdata`, `.data` 0xB94000 (86 MB virtual), `.shader`, `.rsrc`, `.reloc`.
- The EXE is a COPY; analysis never touched the installed EXE or the running game.

### PR 13379 symbols

- Source: LoneGazebo/Community-Patch-DLL PR #13379 ("Make EXE interop easier to add and change", open, head
  `vaderkos/Community-Patch-DLL@ae17ecad6`), file `CvGameCoreDLL_Expansion2/Exe/CvExeAddresses.h`. Names are the
  engine's own symbols from the Linux build, per the PR text.
- `pr13379_symbols.csv`: 12 entries with DX11, DX9 and Tablet VAs (preferred base 0x400000) and byte signatures;
  10 are addresses (3 functions, 3 code locations, 2 vtables, 2 data), 2 are struct offsets.
- Checked against the EXE copy: all 6 signatures match at the DX11 VAs.
- Applied by `ghidra_scripts\ApplyPr13379Symbols.java` via `ghidra_apply_pr13379.cmd` (`-process -noanalysis`):
  "applied 10, skipped 2 (offsets), signature failures 0", saved. Each label has a plate comment
  `PR #13379 <kind>: ... (signature verified)`. The script re-checks each signature in Ghidra before it labels.
  Takes the column as a 2nd arg (`dx11`, `dx9`, `tablet`) for other builds.

## ghidra-mcp (bethington/ghidra-mcp 7.0.0)

- Source: https://github.com/bethington/ghidra-mcp/releases/tag/v7.0.0.
  - `GhidraMCP-7.0.0.zip` SHA-256 `d133c4c869a0d9f605c1c9426bf5d9064772a6947ccecde0f42fbbffecf1d812`
  - `ghidra_mcp_bridge-7.0.0-py3-none-any.whl` SHA-256 `a75050061d0d1514d4b11e6288ea7575c09ab6125a1394c33fe349fb7658bbae`
  - Both match GitHub's release-asset digests (the project publishes no separate hash file).
- Extension: extracted into `%APPDATA%\ghidra\ghidra_12.1.4_PUBLIC\Extensions\GhidraMCP` (the same thing
  File > Install Extensions does). `extension.properties` says `version=12.1.4`.
- Bridge: venv `C:\Users\Art\tools\ghidra-mcp-7.0.0\venv` (anaconda Python 3.11.5), wheel installed with pip
  (mcp 1.30.0, httpx 0.28.1, ...). Entry point `C:\Users\Art\tools\ghidra-mcp-7.0.0\venv\Scripts\bridge-mcp-ghidra.exe`.
- `.mcp.json` at the worktree root registers server `ghidra-mcp` (stdio, absolute path to the bridge,
  `GHIDRA_MCP_URL=http://127.0.0.1:8089`).
- Plugin HTTP server: `http://127.0.0.1:8089` (loopback). Script-running endpoints are off unless
  `GHIDRA_MCP_ALLOW_SCRIPTS=1` is set in Ghidra's environment.
- Verified: the bridge starts over stdio and answers MCP `initialize` + `tools/list` (8 static tools:
  `list_instances`, `connect_instance`, `list_tool_groups`, `load_tool_group`, ...; it logs "No Ghidra instances
  found ... retrying in the background"). NOT verified: the Ghidra side (plugin loaded, `/check_connection`),
  because the first GUI start needs Art to accept Ghidra's User Agreement.

### How Art enables it

1. Run `experiments\shell-gates\tooling\ghidra.cmd`. On the first start accept the User Agreement.
2. If Ghidra says new plugins were found and asks to configure them, say Yes and tick **GhidraMCPPlugin**.
   Otherwise, in the project window: **File > Configure**, under **Utility** click **Configure**, tick
   **GhidraMCPPlugin**, OK. (If it is missing, check **File > Install Extensions** lists GhidraMCP, then restart
   Ghidra.) Do this in the project window, not CodeBrowser, so the server runs as long as Ghidra does.
3. **File > Open Project** > `C:\Users\Art\tools\ghidra-projects\civ5\civ5.gpr`, then double-click
   `CivilizationV_DX11.exe` to open it in CodeBrowser. If asked to analyse: No (it is already analysed).
4. Check: `experiments\shell-gates\tooling\ghidra_mcp_check.cmd` should print
   `{"status": "ok", "server_kind": "gui", "version": "7.0.0", "program": "CivilizationV_DX11.exe"}`.
5. Start Claude Code in the worktree (`...\.claude\worktrees\shell-gates-d8e422`). It asks to approve the project
   MCP server `ghidra-mcp` from `.mcp.json`: approve it. `/mcp` shows its status. The bridge loads the
   `listing,function,program` tool groups on connect and the rest on demand (`search_tools`, `load_tool_group`).
   If Claude Code started before Ghidra, the bridge keeps retrying; or call `connect_instance`.
6. While Ghidra has the project open, the headless launchers fail on the project lock: close it first.

## Sysinternals VMMap, Process Monitor, Process Explorer

- Source: https://download.sysinternals.com/files/VMMap.zip, `.../ProcessMonitor.zip`, `.../ProcessExplorer.zip`.
  No published hashes; every EXE carries a valid Microsoft Corporation Authenticode signature
  (`Get-AuthenticodeSignature` = Valid).
  - `VMMap.zip` SHA-256 `71c9bb73f1ba63f69096cc5ab11aaad4f1969b060931f19c97f7ca6458ca1c1e` (vmmap.exe 3.4)
  - `ProcessMonitor.zip` SHA-256 `97330813853e0e514a11ee16a47d4c6fe90148fae834649839afb30487cc8b0b` (4.13)
  - `ProcessExplorer.zip` SHA-256 `22551d0181dde6cb6af1bc134a190d867d883673f9c35c33039e4e8f9ac75df1` (17.21)
- Launch: `vmmap.cmd` (32-bit `vmmap.exe`, args passed through; `vmmap.cmd -p <pid> out.mmp` writes a snapshot
  with no GUI), `procexp.cmd`, `procmon.cmd`. `-accepteula` is passed (HKCU EULA key).
- Verified: `vmmap.exe -accepteula -p <pid> out.mmp` on a throwaway 32-bit `SysWOW64\PING.EXE` exited 0 and wrote
  a 77 KB `.mmp`. The EULA key for VMMap is now set. Process Monitor needs elevation to run (kernel driver); it
  was not run. Process Explorer was not launched (GUI).
- `vmmap64a.exe`, `procexp64a.exe`, `Procmon64a.exe` are the ARM64 builds; ignore them.

## RenderDoc

- Source: https://renderdoc.org/stable/1.47/RenderDoc_1.47_64.zip (from https://renderdoc.org/builds).
- SHA-256 `b51712e58d40066b34ca30c7869d15f72e4b5c001b2d92232f6783a636d5a3c1`. No hash is published; the zip's
  GPG signature (`RenderDoc_1.47_64.zip.sig`) verifies "Good signature from Baldur Karlsson" with key
  fingerprint `1B03 9DB9 A471 8A2D 699D E031 AC61 2C31 20C3 4695`, the fingerprint printed on the builds page
  (key from https://renderdoc.org/baldurk-pubkey.asc, imported into a throwaway keyring in `_downloads\_gnupg`).
- Launch: `renderdoc.cmd` (qrenderdoc UI) or `renderdoc.cmd cmd <args>` (renderdoccmd).
- Verified: `renderdoccmd.exe --version` = `renderdoccmd x64 v1.47 built from b0e13703...`;
  `x86\renderdoccmd.exe --version` = `renderdoccmd x86 v1.47`; the x86 capture layer is present
  (`x86\renderdoc.dll`, `renderdocshim32.dll`, `renderdoc.json`), Authenticode-valid. No capture was taken.

## MinHook

- Source: https://github.com/TsudaKageyu/minhook, tag `v1.3.4` (latest release, 2025-03-28),
  `git clone --branch v1.3.4 --depth 1`. Commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.
- Source only, nothing built. `include\MinHook.h`, `src\`, `build\` (VS projects), `CMakeLists.txt`.

## 64-bit and 32-bit C++ build path

- VS 2022 Community: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat`
  (present). Windows SDK 10.0.26100.0 (present; 10.0.22621.0 also installed). LLVM: `C:\Program Files\LLVM\bin`,
  clang 19.1.7.
- Test: `d3d11_hello\d3d11_hello.cpp` (D3D11CreateDevice on the default adapter, prints the DXGI adapter name),
  built and run by `d3d11_hello\build_d3d11_hello.cmd`. Exact commands:

  ```bat
  set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.26100.0
  "C:\Program Files\LLVM\bin\clang-cl.exe" -m64 /nologo /EHsc /O2 /W4 d3d11_hello.cpp /Fe build\d3d11_hello_x64.exe /Fo build\d3d11_hello_x64.obj

  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 10.0.26100.0
  "C:\Program Files\LLVM\bin\clang-cl.exe" -m32 /nologo /EHsc /O2 /W4 d3d11_hello.cpp /Fe build\d3d11_hello_x86.exe /Fo build\d3d11_hello_x86.obj
  ```

  (each vcvarsall call in its own `setlocal` block; the `PATH` line only silences vcvarsall's "vswhere.exe is not
  recognized" message.)
- Verified 2026-11-09, both clean with /W4:

  ```
  bitness: 64-bit   adapter: NVIDIA GeForce RTX 4080   feature level: 0xb000   dedicated video memory: 16048 MB
  bitness: 32-bit   adapter: NVIDIA GeForce RTX 4080   feature level: 0xb000   dedicated video memory: 3072 MB
  ```

  The 32-bit process sees `DedicatedVideoMemory` clamped to 3072 MB (DXGI caps it for 32-bit callers).

## Civ 5 Linux build

Not present locally (checked 2026-11-09, nothing downloaded):

- Steam libraries: `C:\Program Files (x86)\Steam` and `D:\SteamLibrary`. Civ 5 (app 8930, build id 4390913) is
  installed only in the first, Windows depot only; no depotcache entries.
- Name search over C: and D: (Windows, Temp and anaconda pruned) for `Civ5XP`, `Civ5XP.sh`, `*CvGameCore*.so*`,
  `libCiv*.so*`: no hits.
- ELF magic scan of every file in the Civ 5 and Civ 5 SDK folders (14,659 files): 0 ELF files.
- The only `.so` files on the Steam libraries belong to other games (Portal 2, Hearts of Iron IV; AoE2DE's `.so`
  files are shaders).

PR 13379 says its names come from the Linux build's symbols, so the author has one; it is not on this machine.
