# Contributing to VP64

This guide is for anyone who wants to change the 64-bit patch, and for the coding agents that work for them. It says
how a change is proposed and merged, the rules every change keeps and how each is checked, what to read and run for
each part of the code, and what a pull request contains.

Read [`README.md`](README.md) first: it says what the patch is, what it is not, and where each part lives. A change
that adds an optional difference to the picture (an *extra*) has its own guide,
[`CONTRIBUTING-extras.md`](CONTRIBUTING-extras.md); this guide covers everything else and does not repeat it.

Paths below are relative to the folder that holds this guide.

## 1. Before you write code

1. **Find out who owns the thing you want to change.** The game's EXE still owns the window, the interface, Lua,
   sound, the network, saves and asset loading; the 64-bit server owns the game DLL and the world renderer. The table
   in [`research/07-architecture-decisions/ownership-end-product.md`](research/07-architecture-decisions/ownership-end-product.md)
   lists every graphics piece with its owner. A change that moves a piece from one side to the other is a design
   change: propose it in an issue before writing it.
2. **Search the notes.** [`research/`](research/README.md) records what the EXE and the DLL do, the designs, the
   measurements, and what was tried and did not work. Search it for the function, file, EXE location or symptom you
   are about to look into. Many dead ends are written down there with the reason.
3. **Build once, unchanged.** [`BUILDING.md`](BUILDING.md) has the toolchain and the one command. You need your own
   copy of the game: nothing of the game is in this folder.
4. **Open an issue for anything larger than a small fix.** Say what the player sees, what the unpatched game does in
   the same situation, and what you intend to change. Agreeing on the approach first avoids a pull request that
   cannot be taken. Bug reports go to the issue tracker the README names; a good report gives the game's build
   number, the Vox Populi version, the patch version, `VP64Options.ini`, and the error report if the patch wrote one.

## 2. What kind of change is it

| Your change | What it is | Where to go |
|---|---|---|
| The patch crashes, hangs, or behaves differently from the unpatched game | A bug in the patch. Fixed without an option. | Sections 3 to 7 |
| A crash or hang that the unpatched game has too | Fixed without an option, if the fix keeps the rules of section 3. | Sections 3 to 7 |
| The unpatched game draws something wrongly and you want the patch to draw it right | An extra of the category *bugfix*. | `CONTRIBUTING-extras.md` |
| New visual polish the game does not have | An extra of the category *visual addition*. | `CONTRIBUTING-extras.md` |
| Faster turns, faster frames, less memory, with the same game and the same picture | A performance change. It needs numbers (rule G8). | Sections 3 to 7 |
| Game rules, balance, AI, the interface's screens | Not this folder. The server runs the same DLL source as the 32-bit game, so these go through the DLL's own contribution process. | The DLL repository |
| A new player option | An option, with one table row and one document entry (rule G9). | Section 4.7 |

## 3. The rules every change keeps

They apply to the project's own changes exactly as to anyone else's.

| # | Rule | How it is checked |
|---|---|---|
| G1 | **The EXE is never modified on disk.** The patch changes the EXE's behaviour only in memory, at run time, and only on EXE builds it recognises. On an unknown build it touches nothing. The base patch does not patch the EXE's instruction bytes: it works through data, import slots, virtual tables and the EXE's own exported behaviour. The EXE's copy-protection check is never patched or answered. A change that needs more than these means is proposed in an issue first, and is never part of the base patch. | Review; the build-table checks of section 4.8. |
| G2 | **Nothing from the EXE or the game's files enters this folder.** No decompiler output, no disassembly listing, no shader, texture, model, sound or text of the game, no picture of the game, no save, log, dump or scene recording. Behaviour that is re-implemented from the EXE is written from the notes' description of what it does, in your own words. Shaders and assets are read from the player's install at run time. | Review of every file the pull request adds. |
| G3 | **The 32-bit DLL stays unchanged.** A 64-bit-only change to the DLL source is behind the compiler's own macros or a `VP_OUT_OF_PROCESS` / `VP64_*` define. With the define off, the compiler sees the same source as before. | The preprocessed source of each changed DLL file, before and after, with the 32-bit build's flags: equal except for line numbers (section 4.6). |
| G4 | **Same game, same saves.** The server plays the same game as the 32-bit DLL, and writes saves in the same format. A save made with the patch loads in the same Vox Populi version without it, and the other way round. Builds of the patch for one Vox Populi version read each other's saves. Nothing is added to a save and nothing changes what multiplayer players send each other. | The same save played before and after the change gives identical game statistics and identical saves (section 5.3). |
| G5 | **No degraded game.** When something the patch's own path needs fails (the server cannot start, a shared texture cannot be opened, the server stops answering), the patch ends the game with an error message and a report. It never goes on with a half-drawn world, and it never quietly hands the drawing back to the EXE for the rest of the session. The patch stays switched on afterwards; the message offers the player "Remove 64bit patch". An extra is the opposite case: it switches itself off and the picture is drawn without it (rule E6 of the extras guide). | A test that takes the input away or stops the other side, and reads the message and the report. |
| G6 | **With every extra off, the picture is the unpatched game's.** The renderer's goal is the same picture, pixel for pixel. A look-alike is accepted only as a stated, temporary gap. A deliberate difference is an extra. | The offline scene tests (section 5.2); a live comparison with both extras category switches at 0. |
| G7 | **A crash fix names its cause.** A crash, hang or assert is closed with the cause found and the fix or the guard for that cause, not with "it did not happen again in N runs". If the cause could not be found, the pull request says so and says what was ruled out. | Review; the cause is added to the notes (section 6). |
| G8 | **A speed or memory claim comes with numbers.** Both arms on the same machine, the same save or scene, the same settings; at least 3 runs per arm for turn times and 5 for frame times; median and range; and why the numbers moved. A difference inside the noise is reported as no difference. | The numbers in the pull request; the notes in `research/08-performance/` say how the project measures. |
| G9 | **One option, one table row, one document entry.** Every key of `VP64Options.ini` has one row in the options table and one entry in [`docs/OPTIONS.md`](docs/OPTIONS.md). Defaults are set by the maintainers. | The options test (section 5.1). |
| G10 | **The player's files stay the player's.** The patch changes the game's own settings files only where it records what it changed and puts it back (the helper's `on` and `off`), and never silently. Its report holds no computer name, user name or personal folder path. The patch's programs open no network connection and touch no process other than the game and their own server. | Review; the helper's and the report's tests. |
| G11 | **Licence and origin.** Everything you contribute is published under GPL-3.0-or-later. Code taken from another project names that project, its path and its version at the top of the file or block, and only if its licence is compatible. No prebuilt third-party binary is added, and no library of the game's middleware. | Review. |

## 4. By part of the code

Each part has a README that describes its design, its build and its tests in detail. This section says only what a
contributor must know before changing it.

### 4.1 The world renderer (`render/`, `server/src/render/`)

- It runs on the server's render thread and is compiled without the DLL's headers. Its only input from the game is
  the event feed (`server/src/feed/srv_feed.h`) and the tables the feed copies. Do not add another way in.
- Develop against recordings, not a running game. The offline scene tools draw a recorded scene again without the
  game (`server/tools/sceneshot/README.md`, [`render/README.md`](render/README.md),
  `research/06-tooling-and-workflows/offline-scene-harness.md`). A live game is needed only for the final check.
- Whatever the server newly draws by default must be replayable offline: its input is part of the recording, or a
  key in the recording's configuration switches it. Otherwise every recorded scene stops matching.
- Recordings hold data read from your copy of the game. Never attach one, or a picture made from one (rule G2).
- Read the piece's note in `research/05-graphics-composition/` first. It says how the EXE draws that piece and what
  the server's version must reproduce.

### 4.2 The server's host side (`server/src/`, outside `render/`)

- The server loads the x64 DLL and supplies the services the EXE supplied to it (files, the database, Lua, the
  script system). `research/03-dll-engine-boundary/server-host.md` lists each service with its contract.
- A call the server does not serve is refused and logged. It is never answered with a guessed value that lets the
  game go on in a wrong state.
- The game thread and the render thread share nothing except the feed. A change that reads a DLL object from the
  render thread is not accepted.

### 4.3 The proxy and the helper (`proxy/`)

- The proxy is a 32-bit DLL that takes the game DLL's place inside the EXE. It links the game's own C runtime
  and runs inside the EXE's process: a fault in it is a crash of the game.
- Its interface code is generated from a model of the DLL's interfaces (`proxy/model/`, `proxy/gen/`). The list of
  calls the proxy may answer without asking the server is audited against the DLL source, and the build refuses a
  list that no longer matches ([`proxy/README.md`](proxy/README.md)). Do not edit generated files by hand.
- Nothing in the proxy may write EXE memory except through the build table (section 4.8).
- The helper (`proxy/helper/`) switches the patch on and off in the game's settings and shows the failure message.
  Rule G10 is mostly about it.

### 4.4 The shell side and the compositor (`shell/`)

- The compositor puts the server's world picture into the EXE's frame at one fixed point of the frame. The notes
  `research/05-graphics-composition/shared-texture-and-compositor.md` and `insertion-point-and-frame-structure.md`
  describe the frame and why the point is where it is.
- The interface the player sees always belongs to the world picture under it: the EXE waits for the server's frame
  of the same camera, however long that takes, and only a server that has stopped ends the wait. Do not add a path
  that shows an older world picture under a newer interface.
- [`shell/README.md`](shell/README.md) has a table of error paths and the test of each. A new error path adds its
  row and its test.

### 4.5 The transport (`ipc/`)

- [`ipc/PROTOCOL.md`](ipc/PROTOCOL.md) is the specification. The two sides compare protocol version, layout hash
  and schema hash when they attach, and refuse each other on a mismatch. The proxy and the server are always built
  and shipped together; no change may rely on two different builds talking to each other.
- A change to a message's layout changes the specification in the same pull request.
- Latency claims follow rule G8; `research/03-dll-engine-boundary/ipc-design-and-measurements.md` has the baseline.

### 4.6 The game DLL

The DLL source is next to this folder, not in it. Two different things are both called "a DLL change":

- **A change to the game** (rules, AI, a fix that the 32-bit game should have too) is an ordinary change to the DLL
  and follows the DLL repository's own process. The patch picks it up when its pinned DLL commit moves
  (`deps/dll.lock`, moved by the maintainers).
- **A change that only the 64-bit build needs** (a type that must not change size, a send site for a render
  message, a hook for the server) follows rule G3. Check it like this:
  1. Build the x64 DLL with your change ([`tools/x64-dll/README.md`](tools/x64-dll/README.md)); a DLL source at
     another commit than the pin is built with `--unlocked`.
  2. For the 32-bit build, compare the preprocessed source of every changed file at the commit before yours and at
     yours, with the 32-bit build's flags. The text must be equal except for line numbers.
  3. Run the game identity check of section 5.3.
- Send sites for render messages have their own rules in section 4 of the extras guide; they apply to every send
  site, not only to those of an extra.

### 4.7 Options (`options/`)

[`options/README.md`](options/README.md), "Adding an option", is the procedure. In short: one row in the table in
`options/src/vp64_options.cpp`, one entry in `docs/OPTIONS.md` in the form of the others, read once at the start,
never re-read during a game. An option that changes the picture is an extra and follows the extras guide instead.
A new option is merged with the default that leaves behaviour as it was.

### 4.8 EXE locations (`exebuild/`, `tools/exebuild/`)

- No source file holds a literal EXE address. Every location the patch uses is a named symbol in the catalogue
  (`exebuild/symbols/`), resolved for each known EXE build by a table that the build generates from your own copy
  of that EXE, and checked at run time against the bytes the table expects before anything is read or written
  there. `research/02-exe-internals/exe-build-table.md` describes the registry and how a symbol is added.
- The generated tables, and any hand-made answers for a build, stay on your machine. Do not commit them, and do not
  paste EXE addresses of other builds into a pull request, an issue or a note.
- A new use of an EXE location says in the pull request what is read or written there, in which state of the game,
  and what happens when the check fails (the feature stays off with one log line; the game runs on).

### 4.9 Build and release tools (`tools/`)

Standard library Python only, no network access except the pinned third-party archives of `deps/third-party.lock`,
and output only into the folder the caller names. A change to a build script is followed by one full
build with the command of `BUILDING.md`.

## 5. Tests

General rules:

- **A bug fix comes with a test that fails without the fix and passes with it.** Say in the pull request that you
  saw it fail.
- **Every check has a negative case.** A test that would also pass if the code did nothing proves nothing.
- **A skipped test is not a passed test.** If a test skipped because your machine lacks an input, say which.
- **Native code runs its tests once under AddressSanitizer** (each builder's `--asan` option; the project uses
  MSVC's sanitizer).
- **Run every builder that compiles a file you changed.** Several test programs compile product sources directly;
  `python -I tools/exebuild/compiled_by.py <file> [<file> ...]` lists them.

### 5.1 What to run for each part

| You changed | Run | Needs |
|---|---|---|
| `ipc/` | `python ipc/build.py --arch both --test` | the toolchain |
| `options/`, `docs/OPTIONS.md` | `python ipc/build.py --arch both --target test_options`, then each `build/<arch>/Release/options/test_options.exe docs/OPTIONS.md <scratch folder>`; `python options/test_asan.py` | the toolchain |
| `models/` | `python models/build.py --test` (`--allow-skip` on a machine without extracted game models) | the toolchain; game files for the full run |
| `render/` | `python render/build.py --test --game-dir <copy of the game> --exe <its CivilizationV_DX11.exe>`, and once with `--asan` | a copy of the game |
| `server/src/`, `proxy/` | the one command of `BUILDING.md` (it builds the server and the proxy and runs the proxy's audit), then the test scripts of the area you touched under `server/tests/` and `proxy/tests/`; each script's header gives its command | the DLL source, a copy of the game |
| `server/src/extras/`, `server/src/feed/` | `server/tests/extras/run_extras_test.py`, `server/tests/feed/run_feed_test.py`, `server/tests/feed/run_render_events_test.py`, each with `--out <dir>` and once with `--asan` (see section 5.4) | the toolchain |
| `shell/` | `cmd /c shell\build.cmd` (set `C5_SHELL_BUILD` to an output folder), then the stand-in tests in `shell/tests/` | the toolchain |
| `tools/x64-dll/`, `tools/exebuild/`, `tools/release/` | `python -I -m pytest -q -p no:cacheprovider <that folder>/tests` | Python |
| the DLL source | section 4.6 and section 5.3 | both DLL builds, a copy of the game |

[`research/06-tooling-and-workflows/runbook.md`](research/06-tooling-and-workflows/runbook.md) is the list of exact
commands, with how long each takes and what it needs; `test-health-on-main.md` beside it lists every test folder.
Both were written on the project's own machines. Read section 5.4 before you copy a command from them.

### 5.2 The picture (rule G6)

The offline scene player draws each recorded scene and compares the result with the server's own pictures stored
in the recording. Section 2.2 of the extras guide has the commands; for a change that is not an extra, the
comparison must hold for every scene with no option set. A change that is meant to alter the picture (a fix of a
difference from the unpatched game) says which scenes change and why, and describes the new pictures in words.

### 5.3 The same game (rule G4)

Required for any change to the DLL source, to how the server hosts the DLL, or to anything on the game thread:

1. Take a save. Load it with the build before your change and play at least 10 turns with the DLL's statistics log
   on. Do the same with your build.
2. The two statistics logs must be identical in every table and column except process measurements, and the saves
   written at the end must be identical.
3. Compare like with like: both runs windowed, or both without a window. A new game is not a substitute for a
   loaded save: two new games never give byte-equal saves.

A change that is meant to alter the course of the game (the server gave the DLL a wrong value and now gives the
right one) says so, shows the first difference in the statistics, and explains why it is the fix and nothing else.

`research/06-tooling-and-workflows/d10-save-identity-test.md` describes the test and its tools.

### 5.4 Tools the notes name that this folder may not carry

The notes and some script headers were written for the project's own test machines. When you follow them:

- A command that starts with `python -I perf/quietlock/quietlock.py run ... --` is the project's lock for a shared
  test machine. Run the command that follows the `--` by itself. Build scripts that look for the lock print one
  warning when it is absent and go on. A few test runners under `server/tests/` still refuse to start without it
  (they print `perf/quietlock/gate.py not found`); treat those as checks you could not run.
- The maintainers' release descriptors (`tools/release/releases/`) are not in this folder: they name the maintainers'
  machine folders. Scripts that read one (`server/run_server.py`, the two `run_ab.py` A/B tests under
  `tools/x64-dll/`) print one warning and then need their data flags: `run_server.py` takes `--game-dir` and, for a
  direct run, `--db`. Or set `C5_RELEASE` to a descriptor file of your own (the format is in
  `tools/release/descriptor.py`).
- Placeholders such as `<t>`, `<inst>` and work folders on fixed drive letters are the project's own. Use any
  folder outside the repository.
- Tools that start, drive and stop a hidden copy of the game, the comparers for statistics logs and saves, the
  check for private content, and a helper the shell's stand-in tests import are not all part of the published
  folder. If a check in this guide needs a tool your copy does not have, do the part you can, and say in the pull
  request exactly which check you could not run. The maintainers run it before the merge. Do not report a check as
  passed that you did not run.

### 5.5 Testing with a live game

- Test on a **copy** of the game, never on the install you play. Tests never write into a game folder.
- Before a test changes the DLL, `config.ini`, `GraphicsSettingsDX11.ini` or the game's interface files in a copy,
  record their hashes, and restore them by hash at the end.
- Never delete, overwrite or move a save. Experiments use copies with their own names.
- Do not stop the game while it is writing an autosave.
- Never create junctions or symbolic links into a game folder.
- Tools come from their official sources. No third-party game binaries.
- In the pull request, name the game's build number, the Vox Populi version, the map and save, and whether the
  game ran windowed.

## 6. The notes

The notes are part of the product: most of this patch was built from them, and the next contributor will start
there. A pull request that learned something updates them in the same change.

| What you learned | Where it goes |
|---|---|
| How the EXE behaves, a new EXE location | `research/02-exe-internals/` (and the symbol catalogue, section 4.8) |
| The boundary between the EXE and the DLL, the proxy, the transport, the x64 DLL | `research/03-dll-engine-boundary/` |
| Memory | `research/04-memory-investigation/` |
| How the EXE draws a piece, how the server draws it, the compositor | `research/05-graphics-composition/` |
| A command you had to work out, a test's inputs, a crash cause and its fix | `research/06-tooling-and-workflows/` (`runbook.md`, `known-crash-causes.md`) |
| A measurement | `research/08-performance/` |

How to write them:

- **Extend the file on the topic.** Start a new file only for a new topic, and add its row to the section's
  `README.md` in the same change.
- **Facts, not a story.** Exact names, sizes, timings, versions and commands. Date every measurement, and name the
  save, the build and the settings it was taken with.
- **Mark confidence:** `[C]` confirmed by reading the code or a dump, `[L]` verified live in the game, `[I]`
  inferred.
- **Correct in place.** When your result changes an older statement, edit that statement, date the correction and
  keep one line of what it replaced. Two notes must never disagree without one saying which is current.
- **Write down what did not work,** with the reason. It saves the next person the same hours.
- **Describe the EXE in your own words** (rule G2): what a routine does, its inputs and its effects. No decompiler
  output, no listing, no pictures.

## 7. The pull request

Open it against the branch that holds this folder. One topic per pull request. The description says:

1. **What was wrong, or what is new,** as the player sees it.
2. **The cause** (rule G7) and what the change does about it.
3. **Which parts it touches,** and for each rule of section 3 it could affect, how you checked it.
4. **What you ran,** with the result of each: the command, the count passed, the machine for any number.
5. **What you did not run or could not verify.** This line is required. "Nothing" is a valid answer only if it is
   true.
6. **Which notes you updated.**

Do not attach pictures of the game, saves, logs, dumps or scene recordings: describe them in words, and keep them
where a maintainer can ask for them.

**If you are an agent, or work with one.** Contributions written by coding agents are welcome on the same terms.
The person who opens the pull request answers for it: they have read the change, and every check the description
lists was really run, on the change as submitted. An agent's summary of a test is not the test's result: quote the
test's own last line.

**What only the maintainers decide:** option defaults and option ids, the pinned DLL commit, which EXE builds and
which Vox Populi version a release supports, protocol version numbers, and releases. A pull request may propose any
of these in its description; it does not change them.
