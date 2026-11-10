# Firaxis Live Tuner (FireTuner2)

Explored 2026-09-16 against the running FireTuner2 (PID 32920). **The game was not running, so FireTuner was
disconnected the whole time.** **Live-tested 2026-09-17 01:47-01:53** against the same FireTuner instance while
connected to a loaded game (turn 317, huge map, game PID 28800): see section 5a for what was run and the exact
outputs. Labels used below:

- **OBSERVED** - seen on this machine (UIA dump, netstat, file contents, a live test).
- **INFERRED** - read from file names, panel XML, or strings in the binaries, but not exercised.
- **UNTESTED** - a recipe step that needs a connected game and has not run yet.

Raw evidence: the UIA dump is `firetuner_uia_tree.txt` and the panel dump is `ltp_dump.txt`, both in the
2026-09-16 session scratchpad (`C:/Users/Art/AppData/Local/Temp/claude/C--Users-Art-Documents-GitHub-Community-Patch-DLL/8e0df880-3f58-4534-b8e3-5329f7c12c90/scratchpad/`).

## 1. What it is

| | |
|---|---|
| Executable | `C:/Program Files (x86)/Steam/steamapps/common/Sid Meier's Civilization V SDK/FireTuner2/FireTuner2.exe` |
| Other files in the folder | `Firaxis.Framework.dll` (shared Firaxis tools library), `P4API.dll` and `p4dn.dll` (Perforce, not used here). No config or panels are stored there. |
| Technology | .NET 2.0 WinForms (loads `mscorwks v2.0.50727` and `System.Windows.Forms`). It runs as a 64-bit process. OBSERVED |
| Window | Title `Firaxis Live Tuner`, AutomationId `frmMainForm` |

What it does:
- It has a **Lua console** that runs a line in a chosen Lua "state" of the running game. A state is a UI context environment such as `WorldView` or `InGame`, or the main state. `print` output shows up in the console.
- It hosts **panels** (`.ltp` XML files). A panel is a canvas of buttons, value fields, lists and tables, and each widget is bound to a Lua snippet that runs in one of the panel's `CompatibleStates`. The stock panels ship in the game's `Debug` folder (section 6).

### FireTuner compared with the DLL Lua channel (`vp_lua.py`)

| | FireTuner console | DLL channel (`PollExternalLuaRequest`) |
|---|---|---|
| Transport | TCP 127.0.0.1:4318, a proprietary protocol | Files plus named events in the cache folder |
| Needs a GUI process | Yes (FireTuner2.exe) | No |
| Return values | A bare expression line is echoed as its values (`Game.GetGameTurn(), Game.GetActivePlayer()` -> `317, 2`); `return x` prints nothing (OBSERVED 2026-09-17) | Return values come back as `[i] value` lines, plus prints and errors |
| States | 159 combo entries, the names match `vp_lua.py --states`; `Main State` is the raw global table (no `Game`/`Players`/`Events`) | `Main` = first thread environment that can see `Game`; `--state _G` = FireTuner's `Main State` |
| Knowing when a command finished | No marker; poll the output text for a printed sentinel | The `VPLuaExecDone` event |
| Round trip (2026-09-17) | ~40-100 ms per line plus sentinel, but ~3.7 s to attach by UIA | 30-60 ms in-process (`elapsed_s`), 0.2-0.3 s wall including Python start |
| Works in the front end (main menu) | Probably: stock panels target `MainMenu` and `OptionsMenu` (INFERRED) | No, only once a game is loaded |
| Output limits | "FireTuner may truncate long output; check Lua.log" (comment in `CPK.Util.Benchmark.lua`) | Written to a file |

Prefer `vp_lua.py`. FireTuner is mainly useful as a source of known-good debug Lua (section 7) and for the
front end.

## 2. How it attaches

- **FireTuner is the TCP client and it retries all the time.** OBSERVED (netstat, 23:06): about every 0.8 s it sends `127.0.0.1:55520 -> 127.0.0.1:4318 SYN_SENT`, then the socket drops back to `Bound`. It needs no click: it connects as soon as something listens on 4318.
- **Game side:** `Documents/My Games/Sid Meier's Civilization 5/config.ini`, section `[Debugging]`:
  - `EnableTuner = 1` (currently 1). Its comment reads "Set to 1 to enable the fire tuner to connect to the game."
  - `SendRemarksToTuner = 0` (sends FRemark output to the tuner).
  - There is no port key. Neither config.ini nor the strings in `CivilizationV_DX11.exe` contain a tuner port setting, so 4318 is a fixed default. That the game listens on 4318 is INFERRED from what FireTuner dials; it was not observed from the game side.
- **Port override on the FireTuner side:** `Connection > Change Connection` opens `ConnectionWnd`, which has a `Port:` text box (`txtPort`) and a `Connect` button. Strings show `127.0.0.1` as the host. A changed port would presumably be saved in `UserSettings.xml`, which today is empty, so the defaults apply (INFERRED). Do not open this dialog on the user's instance.
- **After connecting (OBSERVED 2026-09-17):** the status label reads `Connected`, the window title becomes `Firaxis Live Tuner: Sid Meier's Civilization V`, and the Lua state combo holds **159 entries** (duplicates included: `ConfirmKick` x3, `SaveMapMenu` x4, `SaveMenu` x3, `Demographics` x2, `EmptyPopup` x2), among them `Main State`, `InGame`, `WorldView`, `TechTree`, `CityView` and mod contexts such as `autoplay`, `VPUI_loader`, `EUI_context`. FireTuner loaded 14 panel tabs, the 11 of `DefaultPanels.xml` plus `Game UI`, `Debug` and `Options` from `PanelConfig.xml`, and selected tab index 6 (`Map`; `Lua Console` is index 0), matching `SelectedPanel=6`. The console state was `Main State`. `Connection > Refresh Lua States` asks the game for the list again (not exercised).
- **The console output mirrors the game's whole Lua log** (OBSERVED): after connecting it held ~54 KB of load-time prints from every context (`TechTree: ...`, `autoplay: Game init is complete...`). Lines printed from the console also land in `Logs/Lua.log`, with the same `InGame: ` prefix.
- The exe contains the string "Leaderboard score not posted because Tuner was connected to the game.", so the game does notice a tuner connection.

## 3. Config files (plain XML, readable without reverse engineering)

| File | Content (2026-09-16) |
|---|---|
| `C:/Users/Art/Documents/Firaxis Live Tuner/UserSettings.xml` | `<UserSettings />`, empty: no port or host override |
| `C:/Users/Art/Documents/Firaxis Live Tuner/PanelConfig.xml` | Per-app state, keyed `Civ5`: `LockLuaState=false`, `ConsoleLuaState` empty, `SelectedPanel=6`, `OpenPanels` = `...\Civilization V\Debug\Game UI.ltp`, `Debug.ltp`, `Options.ltp` |
| `.../Sid Meier's Civilization V/Debug/DefaultPanels.xml` | The project's default panel list: Active Player, Audio Logging, Audio, Game, Lua Mem Tracking, Map, Players, Selected City, Selected Unit, Table Browser, Network |
| `.../Sid Meier's Civilization V/Debug/*.ltp` | The 19 stock panels (section 6) |

FireTuner writes `PanelConfig.xml` and `UserSettings.xml` itself: the binary has the strings "Error saving panel config" and
"Unable to save user settings". Do not edit them while it is running. `Admin > Edit Project Panels` edits
`DefaultPanels.xml` in the game folder.

## 4. UIA control map (pywinauto `backend="uia"`)

The whole tree is small, about 40 nodes while disconnected. When connected, each loaded panel is an extra
`TabItem` (OBSERVED 2026-09-17: 16 tab items = `Lua Console`, 14 panels, `* New Panel *`). **Only the selected tab's
page is in the UIA tree**: with the `Map` panel selected, `ConsoleTools`, `txtConsoleInput` and `txtConsoleOutput`
cannot be found (`ElementNotFoundError`), although their HWNDs still exist and still work (section 5a).

### Main window

| Control | control_type | AutomationId | Name / class | Patterns | Notes |
|---|---|---|---|---|---|
| Main form | Window | `frmMainForm` | `Firaxis Live Tuner` | Window | |
| Status strip | StatusBar | `ctrlStatusStrip` | | | |
| Status label (child 0) | Text | (none) | `Disconnected` (or `Connected`) | LegacyIAccessible | **Connection indicator.** Read with `window_text()`. The UI strings in the exe are `Connected` and `Disconnected`. `Firaxis Live Tuner: ` is probably the window-title prefix while connected (INFERRED). |
| Menu bar | MenuBar | `MainMenu` | | | |
| Tab control | Tab | `ctrlMainFormTabs` | SysTabControl32 | Selection | |
| Tab: console | TabItem | (none) | `Lua Console` | SelectionItem | Selected at start and at end of the 2026-09-16 session. **`SelectionItem.Select()` on any tab brings FireTuner to the foreground** (OBSERVED 2026-09-17: the foreground window changed from the user's app to FireTuner). Don't use it while someone is at the machine; drive the console by HWND instead (section 5a). |
| Tab: new panel | TabItem | (none) | `* New Panel *` | SelectionItem | **Trap:** selecting it opens a modal Panel Builder (see Dialogs). |
| Console page | Pane | `tabLuaConsole` > `LuaConsole` | | | |
| Console toolbar | ToolBar | `ConsoleTools` | `Console Tools` | | |
| **Lua state selector** | ComboBox | **the HWND in decimal** (e.g. `1117070`; changes every run) | `WindowsForms10.COMBOBOX...` | Value | Internal name `cmbConsoleLuaState`. Style DropDownList (CBS type 3). **0 items while disconnected, 159 while connected (2026-09-17).** Items are plain strings; read them with win32 `ComboBoxWrapper(hwnd).item_texts()`. Find it by type under `ConsoleTools`, never by id. The HWND stayed `1117070` for the life of the FireTuner process (same value on 09-16 and 09-17). |
| Lock state | CheckBox | HWND (e.g. `330768`) | `Lock Lua State` | Toggle, Invoke | Unchecked. OBSERVED 2026-09-17: with it unchecked, selecting the `Map` panel tab (state `WorldView`) left the console on `Main State`, so panel activation does not (always) move the console state. |
| Clear output | Button (ToolStrip item, no HWND) | (none) | `Clear Output` | Invoke | |
| **Console input** | Edit | **`txtConsoleInput`** | `WindowsForms10.EDIT...` | **Value** | Single-line. `ValuePattern.SetValue` works (TESTED). |
| **Console output** | Edit | **`txtConsoleOutput`** | `WindowsForms10.RichEdit20W...` | **Value** (no TextPattern) | Read-only (`ES_READONLY`). ValuePattern returns `\r` line ends. Win32 `WM_GETTEXT` returns `\r\n`. Start text: `Working late?` |

### Menus
All menu items have the Invoke pattern. The dropdown items are in the tree even while collapsed, with `vis=False`.

| Menu | Items (effect) |
|---|---|
| File | New Panel, Open Panel, Save Panel, Save Panel As..., Exit. **Do not invoke:** they open dialogs, write files or close the app. |
| Connection | Change Connection (opens `ConnectionWnd`, a modal with a port box), Force Disconnect, Refresh Lua States (asks the game for its state list; safe when connected) |
| Admin | Edit Project Panels (`ProjectPanelsManager`, edits `DefaultPanels.xml`). **Do not invoke.** |
| Help | About Firaxis Live Tuner... |

### Dialogs seen (OBSERVED)

- **Panel Builder** (modal, owned by the main form). It opens when the `* New Panel *` tab is selected.
  - Controls: `PanelBuilder` window, `txtName` Edit, `lstLuaStates` List (SysListView32; the states the panel is compatible with), `m_btnEnterAction` / `m_btnExitAction` buttons, `btnOK`, `btnCancel`.
  - `SelectionItem.Select()` on that tab **blocks the calling UIA thread until the dialog closes**. Close it without side effects by posting `BM_CLICK` (0x00F5) to `btnCancel`.
- **Disconnected message box**. It appears when a command is submitted while disconnected.
  - Class `#32770`, empty title, owned by the main form. Static text "Can't run a command while disconnected"; `OK` button with control id 2.
  - It takes the foreground. Dismiss it by posting `BM_CLICK` to the OK button.

### Rules for automating it
- Anything that can open a modal (tab selection, menu Invoke, button Invoke) should be sent with `PostMessage`, or run with a timeout. Synchronous UIA calls wait for the modal to close.
- Look controls up by AutomationId where one exists. For the combo box and checkbox, look them up by control type or title under `ConsoleTools`: their ids are HWNDs.
- The window is on a monitor at negative X. Do not use screen coordinates; nothing in the recipe below needs them.

## 5. Recipe: driving the console from Python

What each step has been checked against:

| Step | How | Status |
|---|---|---|
| Attach | `Application(backend="uia").connect(process=pid)`, then `window(auto_id="frmMainForm")` | TESTED (3.7 s to resolve the controls on 2026-09-17) |
| Check connection | Text of `ctrlStatusStrip` child 0 == `"Connected"` | TESTED 2026-09-17: `Connected` |
| Make sure the console tab is shown | `SelectionItem.Select()` on TabItem `Lua Console` (returns in 0.04 s) | TESTED, but **it steals the foreground** (2026-09-17). Needed only to find the console controls through UIA; skip it when you already have the HWNDs. |
| List states | win32 `ComboBoxWrapper(combo_hwnd).item_texts()` | TESTED 2026-09-17: 159 names while connected, [] while disconnected |
| Select a state | `ComboBoxWrapper.select(name)`: `CB_SETCURSEL`, then `CBN_SELENDOK` and `CBN_SELCHANGE` sent to the parent. WinForms reflects these to the combo's `SelectedIndexChanged`. | TESTED 2026-09-17: `select("InGame")` took 0.016 s, and the next command really ran in InGame (`print(ContextPtr:GetID())` printed `InGame: InGame`); `select("Main State")` switched back. No focus change. `select` refuses a hidden combo (`verify_actionable`), so the console tab must be showing. |
| Type a command | `ValuePattern.SetValue(line)` on `txtConsoleInput`, or plain `SendMessageW(input_hwnd, WM_SETTEXT, 0, line)` | TESTED (ValuePattern 09-16; WM_SETTEXT 09-17, also while the console tab is hidden) |
| Submit | `PostMessage(input_hwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001)`, then `WM_KEYUP` with `0xC01C0001` (no focus, no mouse) | TESTED 2026-09-17 while connected: the line runs and the input box clears within ~0.03 s. Works with the console tab hidden too. |
| Read output | `WM_GETTEXT` on `txtConsoleOutput` (`HwndWrapper(hwnd).window_text()`), or ValuePattern `CurrentValue` | TESTED 2026-09-17 (WM_GETTEXT, CRLF, also while hidden) |
| Know when it finished | Submit `print("<sentinel>")` after the line and poll for the **printed** sentinel line, not its `> ` echo | TESTED 2026-09-17, with a correction: the echo `> print("@@ft-done-x")` appears in the output at once, before the game has run anything, so matching the token anywhere returns early and hands you the previous command's output. Match only lines that do not start with `>`. |

Guard against the modal: check the status before submitting. Tested: the class below raises `RuntimeError` while
disconnected, and no dialog appears. A tested copy is `firetuner_console.py` in the 2026-09-16 scratchpad.

```python
import ctypes, time, uuid
import psutil
from pywinauto import Application
from pywinauto.controls.hwndwrapper import HwndWrapper
from pywinauto.controls.win32_controls import ComboBoxWrapper
from pywinauto.uia_defines import get_elem_interface

WM_KEYDOWN, WM_KEYUP, VK_RETURN = 0x0100, 0x0101, 0x0D
user32 = ctypes.windll.user32

def firetuner_pid():
    for p in psutil.process_iter(["name"]):
        if (p.info["name"] or "").lower() == "firetuner2.exe":
            return p.pid
    raise SystemExit("FireTuner2.exe is not running")

class FireTunerConsole:
    def __init__(self, pid=None):
        app = Application(backend="uia").connect(process=pid or firetuner_pid())
        m = self.main = app.window(auto_id="frmMainForm", control_type="Window")
        self.status_bar = m.child_window(auto_id="ctrlStatusStrip", control_type="StatusBar").wrapper_object()
        self.tabs = m.child_window(auto_id="ctrlMainFormTabs", control_type="Tab").wrapper_object()
        tools = m.child_window(auto_id="ConsoleTools", control_type="ToolBar")
        self.combo = ComboBoxWrapper(tools.child_window(control_type="ComboBox").wrapper_object().handle)
        self.lock_checkbox = tools.child_window(title="Lock Lua State", control_type="CheckBox").wrapper_object()
        self.clear_button = tools.child_window(title="Clear Output", control_type="Button").wrapper_object()
        self.input = m.child_window(auto_id="txtConsoleInput", control_type="Edit").wrapper_object()
        self.output_hwnd = m.child_window(auto_id="txtConsoleOutput", control_type="Edit").wrapper_object().handle

    def status(self):            return self.status_bar.children()[0].window_text()   # "Connected"/"Disconnected"
    def connected(self):         return self.status() == "Connected"
    def lua_states(self):        return self.combo.item_texts()                      # [] while disconnected
    def output(self):            return HwndWrapper(self.output_hwnd).window_text()  # CRLF line ends
    def current_state(self):
        i = self.combo.selected_index()
        return self.combo.item_texts()[i] if i >= 0 else None
    def select_state(self, name): self.combo.select(name)                             # TESTED 2026-09-17

    def selected_tab(self):
        for t in self.tabs.children(control_type="TabItem"):
            if get_elem_interface(t.element_info.element, "SelectionItem").CurrentIsSelected:
                return t.window_text()

    def select_tab(self, name):    # brings FireTuner to the foreground (2026-09-17)
        if name == "* New Panel *":
            raise ValueError("'* New Panel *' opens a modal Panel Builder and blocks the UIA call")
        t = next(t for t in self.tabs.children(control_type="TabItem") if t.window_text() == name)
        get_elem_interface(t.element_info.element, "SelectionItem").Select()

    def submit(self, line):
        if not self.connected():   # otherwise FireTuner pops a modal "Can't run a command while disconnected"
            raise RuntimeError("FireTuner is not connected to the game")
        user32.SendMessageW(self.input.handle, WM_SETTEXT, None, line)       # or ValuePattern.SetValue
        user32.PostMessageW(self.input.handle, WM_KEYDOWN, VK_RETURN, 0x001C0001)
        user32.PostMessageW(self.input.handle, WM_KEYUP, VK_RETURN, 0xC01C0001)

    def run(self, line, state=None, timeout=15.0):        # TESTED 2026-09-17
        if state is not None and self.current_state() != state:
            self.select_state(state); time.sleep(0.3)
        start = len(self.output())
        token = "@@ft-done-" + uuid.uuid4().hex[:8]
        self.submit(line)
        for _ in range(100):                                 # FireTuner clears the box once it took the line
            if HwndWrapper(self.input.handle).window_text() == "": break
            time.sleep(0.01)
        self.submit('print("%s")' % token)                   # separate line: still arrives if `line` errors
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.output()[start:]
            # The "> print(...)" echo shows up at once; only the printed line means the game got there.
            if any(token in l and not l.startswith(">") for l in chunk.splitlines()):
                return chunk
            time.sleep(0.02)
        raise TimeoutError("no sentinel; output so far: %r" % self.output()[start:])
```
with `WM_SETTEXT = 0x000C` and
`user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_wchar_p]` at the top.

Notes:
- Keep each command to one line. The input is a single-line EDIT; join statements with `;`.
- If "Clear Output" was pressed, or the output was truncated, `start` is wrong. Clear first (`clear_button.invoke()`, UNTESTED) when you need clean capture.
- The console's state did not change when the `Map` panel tab was activated with `Lock Lua State` unchecked (OBSERVED 2026-09-17); other panels were not tried.

## 5a. Live test, 2026-09-17 (connected, turn 317)

Game PID 28800 (VP release DLL, huge map, human player 2), FireTuner PID 32920. Everything below was sent by
`WM_SETTEXT` + posted Enter, and read by `WM_GETTEXT`; no key or mouse input.

**Exact output** (CRLF shown as line breaks; every printed line starts with a space, then `<State>: ` for UI states and
nothing for `Main State`):

| State | Line sent | Console output after the echo `> <line>` |
|---|---|---|
| Main State | `print(Game.GetGameTurn())` | `Runtime Error: [string "_cmdr = {print(Game.GetGameTurn())}"]:1: attempt to index global 'Game' (a nil value)` + `stack traceback:` |
| Main State | `print(type(Game), type(Players), type(GameInfo), type(UI), type(Events))` | ` nil	nil	table	table	nil` |
| Main State | `print(ContextPtr)` | ` nil` |
| InGame | `print(Game.GetGameTurn())` | ` InGame: 317` |
| InGame | `print(ContextPtr:GetID())` | ` InGame: InGame` |
| InGame | `Game.GetGameTurn(), Game.GetActivePlayer()` | `317, 2` (no prefix) |
| Main State | `GameInfo.Units.UNIT_WARRIOR.Combat` | `8` |
| Main State | `1+1` | `2` |
| Main State | `for i=1,2 do print(i) end` | ` 1` / ` 2` |
| Main State | `local a = 5; print(a)` | ` 5` |
| Main State | `return 7` | nothing |
| Main State | `error('boom')` | `Runtime Error: [string "_cmdr = {error('boom')}"]:1: boom` + traceback |

What this shows:
- **How the console runs a line:** it first compiles `_cmdr = {<line>}` and prints the table's values (so a bare
  expression list shows its values); if that does not compile it runs the line as a plain chunk (the `for` and `local`
  lines worked). Runtime errors are reported against the `_cmdr = {...}` chunk. `return x` runs but shows nothing.
  `_cmdr` was not visible afterwards from `vp_lua.py` in either `_G` or InGame.
- **`Main State` is the raw global table, where the game-core API is not registered:** `Game`, `Players` and `Events`
  are nil; `GameInfo` and `UI` are tables. It matches `vp_lua.py --state _G` exactly (same five types). For game
  calls pick `InGame` (or `WorldView`, ...) in FireTuner; `vp_lua.py`'s default `Main` already picks a thread
  environment that has `Game`.
- **The sentinel must be matched on its printed line.** The echo of the sentinel line appears before the game has run
  the first line; the first run of this recipe matched on the echo and returned each command's output one command late.
- **Timing:** 40-100 ms from submit to printed sentinel (a 3,000,000-iteration loop included); state switch 16-19 ms;
  UIA attach 3.7 s. `vp_lua.py` for the same `print(Game.GetGameTurn())`: `317`, `ms=0` in game, 0.03-0.06 s
  `elapsed_s`, 0.2-0.3 s wall per call including Python start-up.
- **Driving it while the console tab is hidden works:** with the `Map` panel tab selected, WM_SETTEXT/Enter to the
  cached input HWND (199912) and WM_GETTEXT on the output HWND (199898) ran lines and returned output, and the
  foreground window did not change. The HWNDs must be found once while the console tab is showing (UIA hides the
  page otherwise). A tested copy is `livetest/ft_raw.py` in the 2026-09-16 session scratchpad
  (`python ft_raw.py <input_hwnd> <output_hwnd> "<line>" ...`).
- **Foreground side effect:** `SelectionItem.Select()` on the `Lua Console` tab made FireTuner the foreground window
  (it had been the user's Claude window). Selecting the `Map` tab back afterwards changed nothing more.

Verdict: the console works as a second channel and is the only one that can reach UI states by FireTuner's own
names, but `vp_lua.py` is faster to attach, returns values without printing, and needs no GUI. Use FireTuner mainly
for the front end (untested) and for its stock panel Lua.

## 6. Panel files (`.ltp`)

These are XML-serialised `PanelData`. Schema, as seen in the stock files:

```
PanelData
  Name, App (Civ5), EnterAction (Lua run when the tab is entered), ExitAction
  CompatibleStates/string*                 - Lua state names; the panel runs in one of these
  Actions/ActionData*                      - Name, Action (Lua chunk), Location{X,Y}        -> button
  StringControls|IntegerControls|FloatControls|BooleanControls/<T>ControlData*
                                           - Name, GetFunction "function() ... end",
                                             SetFunction "function(value) ... end", Location, DefaultValue
  TableViews/TableViewData*                - Table (a global Lua table name), Fixed, OnRefresh (Lua), Location, Size
  SelectionLists/SelectionListData*        - Name (column spec "Col:width;Col2;..."), PopulateList
                                             "function() return {rows} end" (rows are ';'-separated),
                                             OnSelection "function(selection) ... end", Sorted
  MultiselectLists/MultiselectListData*    - PopulateList returns {Text=, Selected=} items, OnSelected, OnDeselected
  DataViews, StatTrackers                  - empty in every stock panel
```

Stock panels. All are in `C:/Program Files (x86)/Steam/steamapps/common/Sid Meier's Civilization V/Debug/`; no `.ltp`
exists in the SDK, the modpack or `Assets`. "Default" means listed in `DefaultPanels.xml`.

| File | Panel name | CompatibleStates | Default | Contents |
|---|---|---|---|---|
| Active Player.ltp | Active Player | WorldView | yes | Tech grants and removals, gold, culture and faith, war declaration, maintenance recalculation; research, gold and culture fields |
| Audio Logging.ltp | Audio Logging | Main State | yes | Audio trigger log |
| Audio.ltp | Audio | Main State, InGame | yes | Music and volume knobs, soundscape lists |
| Debug.ltp | Debug | Main State | no | GC, **SQL, Lua, system and DLL-context memory usage readouts**, SQL statement list |
| File System.ltp | File System | OptionsMenu | no | Engine search paths |
| Game UI.ltp | Game UI | WorldView | no | Open popups list |
| Game.ltp | Game | InGame | yes | AI autoplay N turns, force a victory, "return as" player |
| Great People.ltp | Great People | WorldView | no | Named great person plopper |
| HelloWorld.ltp | Hello Panel World | MainMenu | no | `print("Hello World");` |
| Lua Mem Tracking.ltp | Lua Mem Tracking | Main State | yes | Lua allocation tracking (needs a memory-tracker build; see section 8) |
| Map.ltp | Map | WorldView | yes | Reveal map, ploppers (unit, city, resource, improvement), feature and route painting, one of each unit |
| Network.ltp | Network (App empty) | MainMenu, InGame | yes | MP connections, forced resync |
| Options.ltp | Options | WorldView | no | Game options and victory toggles, max turns |
| Players.ltp | Players | WorldView | yes | Player table |
| Selected City.ltp | Selected City | CityView | yes | Population, food, WLTKD, damage, AI focus, buildings multiselect, city list |
| Selected Unit.ltp | Selected Unit | DebugMenu | yes | Damage and XP of the selected unit, embark, animation action |
| Table Browser.ltp | Table Browser | 46 distinct states (48 entries, duplicates included) | yes | Generic table view with an empty `Table` |
| Unit State Selector.ltp | Unit State Selector | DebugMenu | no | Unit animation state machine jump |

State names seen across panels: `Main State`, `InGame`, `WorldView`, `CityView`, `DebugMenu`, `OptionsMenu`,
`MainMenu`, plus Table Browser's list: `WorldPicker, MultiplayerDebug, GridExamples, UITestMenu, LoadGame, StagingRoom,
LANLobby, InternetLobby, MultiplayerSelect, YieldIconManager, ResourceIconManager, UnitFlagManager, CityBannerManager,
UnitPanel, EnemyUnitPanel, MiniMapPanel, PlotHelpText, NotificationPanel, ActionInfoPanel, DiplomacyPopup,
DiploAndAdvisors, Tutorial, UnitMemberOverlay, SpecialistsPopup, TechPopup, TechPanel, TopPanel, ProductionPopup,
CityPurchasePopup, GenericPopup, SocialPolicyPopup, NotificationLogPopup, MinorCivsListPopup, GraphicsPanel, LoadMenu,
GameMenu, DiscussLeader, DiscussionDialog, LeaderHeadRoot`. FireTuner's `Main State` is the raw global table,
the same as `vp_lua.py --state _G`, **not** `vp_lua.py`'s default `Main` (which is a thread environment that can see
`Game`); the other names match `--state <StateName>` (OBSERVED 2026-09-17, section 5a). So the `Main State` panels
(Debug, Audio, Lua Mem Tracking) can use `collectgarbage`, `DB`, `UI`, `GameInfo`, `GameDefines`, `Locale` and
`OptionsManager`, but not `Game`, `Players`, `Teams`, `Map`, `PreGame`, `GameInfoTypes`, `Events` or `LuaEvents` (checked
with `vp_lua.py --state _G`).

## 7. Operations offered by the stock panels (exact Lua)

Whitespace is normalised. Everything else is verbatim from the `.ltp` files. "State" is the panel's
`CompatibleStates`: run it there with `vp_lua.py --state <State>`, or in the matching FireTuner state. None of this
was run during this exploration. Name check: **every game-core method these snippets call is registered in the VP
DLL's Lua bindings** (`Method(...)` in `CvLuaPlayer/Team/TeamTech/Game/Map/Plot/City/Unit.cpp`). The VP argument
differences that matter are flagged as VP notes.

### 7.1 Active Player (state `WorldView`)

| Operation | Lua |
|---|---|
| 1000 Gold | `Players[Game.GetActivePlayer()]:ChangeGold(1000);` |
| 1000 Culture | `Players[Game.GetActivePlayer()]:ChangeJONSCulture(1000);` |
| 100 Faith | `Players[Game.GetActivePlayer()]:ChangeFaith(100);` |
| Player 1 DoW | `Teams[1]:DeclareWar(0);` |
| Gold field (get / set) | `return Players[Game.GetActivePlayer()]:GetGold();` / `Players[Game.GetActivePlayer()]:SetGold(value);` |
| Culture field (get / set) | `return Players[Game.GetActivePlayer()]:GetJONSCulture();` / `Players[Game.GetActivePlayer()]:SetJONSCulture(value);` |
| Current Research ID (get / set) | `local pPlayer = Players[Game.GetActivePlayer()]; local eCurrentTech = pPlayer:GetCurrentResearch(); return eCurrentTech;` / `Players[Game.GetActivePlayer()]:PushResearch(value, 1);` |
| Research Progress (get / set) | `... local pTeamTechs = pTeam:GetTeamTechs(); local eCurrentTech = pPlayer:GetCurrentResearch(); return pTeamTechs:GetResearchProgress(eCurrentTech);` / `... pTeamTechs:SetResearchProgress(eCurrentTech, value);` |

**Grant All Techs** uses the raw TeamTechs flag:
```lua
local pPlayer = Players[Game.GetActivePlayer()];
local pTeam = Teams[pPlayer:GetTeam()];
local pTeamTechs = pTeam:GetTeamTechs();
local iTechLoop = 0;
local pTechInfo = GameInfo.Technologies[iTechLoop];
while( pTechInfo~= nil ) do
   pTeamTechs:SetHasTech(iTechLoop, true);
   iTechLoop = iTechLoop + 1;
   pTechInfo= GameInfo.Technologies[iTechLoop];
end
```
"Remove All Techs" is the same with `false`. The "(All Players)" variants wrap the body in
`for i = 0, GameDefines.MAX_CIV_PLAYERS-1, 1 do if Players[i]:IsEverAlive() then ... end end` with
`pPlayer = Players[i]`.

**`<Era>` Era Techs** buttons (Ancient N=0, Classical 1, Medieval 2, Renaissance 3, Industrial 4, Modern 5,
Future 6) use the full team call:
```lua
local pPlayer = Players[Game.GetActivePlayer()];
local pTeam = Teams[pPlayer:GetTeam()];
local iTechLoop = 0;
local pTechInfo = GameInfo.Technologies[iTechLoop];
while( pTechInfo~= nil ) do
   if (GameInfoTypes[pTechInfo.Era] <= N) then
      pTeam:SetHasTech(iTechLoop, true);
   end
   iTechLoop = iTechLoop + 1;
   pTechInfo= GameInfo.Technologies[iTechLoop];
end
```
"(All Players)" variants exist for Classical, Renaissance and Future.

**Add One Tech** grants the quickest tech the player can research:
```lua
local player = Players[Game.GetActivePlayer()];
local team = Teams[player:GetTeam()];
local quickestToResearchTech = nil;
local quickestResearchTurnsLeft = nil;
for tech in GameInfo.Technologies() do
  if(player:CanResearch(tech.ID) and not team:IsHasTech(tech.ID)) then
    local researchTurnsLeft = player:GetResearchTurnsLeft(tech.ID, false);
    if(quickestResearchTurnsLeft == nil or researchTurnsLeft < quickestResearchTurnsLeft) then
      quickestToResearchTech = tech;
      quickestResearchTurnsLeft = researchTurnsLeft;
    end
  end
end
if(quickestToResearchTech ~= nil) then
  team:SetHasTech(quickestToResearchTech .ID, true);
end
```

**Recalc Building Maint:**
```lua
local pPlayer = Players[Game.GetActivePlayer()];
pPlayer:SetBaseBuildingGoldMaintenance(0);
for pCity in pPlayer:Cities() do
  local iLoop = 0;
  local pInfo = GameInfo.Buildings[iLoop];
  while( pInfo ~= nil ) do
     if (pCity:IsHasBuilding(iLoop)) then
        pPlayer:ChangeBaseBuildingGoldMaintenance(pInfo.GoldMaintenance);
     end
     iLoop = iLoop + 1;
     pInfo = GameInfo.Buildings[iLoop];
  end
end
```

VP notes (from the binding source):
- `Team:SetHasTech` in VP is `(tech, bNewValue, ePlayer, bFirst, bAnnounce[, bNoBonus=false])` and calls `CvTeam::setHasTech`. With only 2 arguments, `ePlayer` reads as `lua_tointeger(nil)` = **player 0**. Pass `pPlayer:GetID(), false, false` explicitly.
- `TeamTechs:SetHasTech(tech, bool)` is `CvTeamTechs::SetHasTech`. It only flips the flag, sets last-tech-acquired and fires the `TeamSetHasTech` hook, **without** the team's tech processing. So "Grant All Techs" differs from the era buttons.
- `pTeamTechs:SetHasTech` is also a quick way to put a team in an inconsistent state. Prefer `Team:SetHasTech` for tests.

### 7.2 Game (state `InGame`)

| Operation | Lua |
|---|---|
| Autoplay 1 / 5 / 10 / 50 / 100 / 150 / 200 / 300 | `Game.SetAIAutoPlay(N,g_ReturnAfterAutoplayPlayer)` |
| Stop AutoPlay | `Game.SetAIAutoPlay(1,g_ReturnAfterAutoplayPlayer)` |
| Win Game - Time / Tech / Domination / Culture / Diplomacy | `Game.SetWinner(0, 0);` / `(0, 1)` / `(0, 2)` / `(0, 3)` / `(0, 4)` (team 0; victory index in stock order TIME, SPACE_RACE, DOMINATION, CULTURAL, DIPLOMATIC; prefer `GameInfo.Victories.VICTORY_X.ID`) |
| Autoplay Turns field (set) | `Game.SetAIAutoPlay(value)` (its GetFunction calls `Game.GetAIAutoPlay()` but does not `return` it) |

The "Return After Autoplay As..." list sets the global `g_ReturnAfterAutoplayPlayer` (`-1` for OBSERVER, otherwise a player
index) in the `InGame` state. VP: `CvGame::setAIAutoPlay(int iNewValue, PlayerTypes eReturnAsPlayer)` goes through `BasicLuaMethod`, so a nil
second argument becomes player 0 (INFERRED from the `BasicLuaMethod` conversion).

### 7.3 Map (state `WorldView`)

These rely on globals defined in `WorldView.lua` (VP override at
`VP_MODPACK/Mods/(2) Vox Populi/Core Files/Overrides/WorldView.lua`, lines 249-428):
`g_UnitPlopper{UnitType, Embarked, UnitNameOffset, Plop(plot), Deplop(plot)}`,
`g_ResourcePlopper{ResourceType, ResourceAmount}`, `g_ImprovementPlopper{ImprovementType, Pillaged}`, `g_CityPlopper`, and
`g_PlopperSettings{Player, Plopper, EnabledWhenInTab}`. `INTERFACEMODE_DEBUG` with debug item ID1 = 6 makes a left click on the
map call `g_PlopperSettings.Plopper.Plop(plot)` and a right click call `Deplop(plot)`. The other ID1 values:
0 city, 1 unit (ID2 = unit id), 2 improvement, 3 route, 4 feature, 5 resource (amount 5).

| Operation | Lua |
|---|---|
| Reveal Terrain | `local pPlot; for iPlotLoop = 0, Map.GetNumPlots()-1, 1 do pPlot = Map.GetPlotByIndex(iPlotLoop); pPlot:SetRevealed(Game.GetActiveTeam(), true); end Map.UpdateDeferredFog();` |
| Reveal/Refresh Map (terrain and visibility) | see the block below |
| Add Forest (click mode) | `UI.SetInterfaceMode(InterfaceModeTypes.INTERFACEMODE_DEBUG); UI.SetInterfaceModeDebugItemID1(4); UI.SetInterfaceModeDebugItemID2(5);` |
| Remove Feature (click mode) | same with `ID1(4)`, `ID2(-1)` |
| Add Road / Add RR / Remove Route (click mode) | same with `ID1(3)` and `ID2(0)` / `(1)` / `(-1)` |
| Toggle Strategic View | `ToggleStrategicView()` |
| Plop a unit where you click | `g_UnitPlopper.UnitType = GameInfo.Units["UNIT_" .. name].ID` (Units list OnSelection), `g_PlopperSettings.Player = i` (Players list), then Unit Plopper Enabled = `UI.SetInterfaceMode(InterfaceModeTypes.INTERFACEMODE_DEBUG); UI.SetInterfaceModeDebugItemID1(6); g_PlopperSettings.Plopper = g_UnitPlopper; g_PlopperSettings.EnabledWhenInTab = true;` |
| City / Resource / Improvement plopper | the same enable block with `g_CityPlopper` / `g_ResourcePlopper` / `g_ImprovementPlopper`. The Resources list sets `g_ResourcePlopper.ResourceType = i`, the Improvements list sets `g_ImprovementPlopper.ImprovementType = i`, the "Res. Amount" field sets `g_ResourcePlopper.ResourceAmount`, and the booleans set `g_ImprovementPlopper.Pillaged` / `g_UnitPlopper.Embarked`. |
| Disable a plopper | `UI.SetInterfaceMode(InterfaceModeTypes.INTERFACEMODE_SELECTION); g_PlopperSettings.EnabledWhenInTab = false;` |
| Add One Of Each Unit | `iUnitTypeCount = 0 while GameInfo.Units[iUnitTypeCount] ~= nil do iUnitTypeCount = iUnitTypeCount + 1; end iUnitTypeCount = iUnitTypeCount - 1; for iUnitType = 0, iUnitTypeCount do g_UnitPlopper.UnitType = iUnitType; kPlot = Map.GetPlotByIndex(iUnitType); g_UnitPlopper.Plop( kPlot ); end` |
| Setup Opposing Units | Every unit type for player 0 in columns x = 0, 2, 4... and for player 1 in x = 1, 3, 5... (y walks the map height), each via `g_UnitPlopper.Plop(Map.GetPlot(iX, iY))`; restores `g_PlopperSettings.Player` afterwards |

**Reveal/Refresh Map:**
```lua
local pPlot;
for iPlotLoop = 0, Map.GetNumPlots()-1, 1 do
   pPlot = Map.GetPlotByIndex(iPlotLoop);
   if (pPlot:GetVisibilityCount(Game.GetActiveTeam()) > 0) then
      pPlot:ChangeVisibilityCount(Game.GetActiveTeam(), -1, -1, true);
   end
   pPlot:SetRevealed(Game.GetActiveTeam(), false);
   pPlot:ChangeVisibilityCount(Game.GetActiveTeam(), 1, -1, true);
   pPlot:SetRevealed(Game.GetActiveTeam(), true);
end
```

What the ploppers call directly, from `WorldView.lua`. Use these without the click mode:
- Unit: `player:InitUnit(unitType, x, y)` or `player:InitUnitWithNameOffset(unitType, nameOffset, x, y)`, then `unit:Embark()` if embarked.
- Remove units: `unit:Kill(true, -1)`.
- Resource: `plot:SetResourceType(type, amount)`; remove with `plot:SetResourceType(-1)`.
- Improvement: `plot:SetImprovementType(type)` then `plot:SetImprovementPillaged(true)`; remove with `plot:SetImprovementType(-1)`.
- City: `player:InitCity(x, y)`; remove with `plot:GetPlotCity():Kill()`.

VP argument lists: `InitCity(x, y[, bBumpUnits=1, bInitialFounding=1, eReligion])`; `InitUnit(type, x, y[, eUnitAI, eFacing, bHistoric=true])`;
`ChangeVisibilityCount(team, change, seeInvisible, bInformExploration, bAlwaysSeeInvisible[, unit])`;
`SetRevealed(team, bValue[, bTerrainOnly, eFromTeam, unit])`.

### 7.4 Great People (state `WorldView`)

The three lists (Great Artists, Great Musicians, Writers) select a unique name:
```lua
local unit = GameInfo.Units["UNIT_ARTIST"];   -- UNIT_MUSICIAN / UNIT_WRITER
local unitNames = {}; local offset = 0;
for row in DB.Query([[select UniqueName from Unit_UniqueNames where UnitType = "UNIT_ARTIST" order by rowid]]) do
  unitNames[row.UniqueName] = offset; offset = offset + 1;
end
local nameOffset = unitNames["TXT_KEY_GREAT_PERSON_" .. tostring(selection)] or 0;
if unit ~= nil then g_UnitPlopper.UnitType = unit.ID; g_UnitPlopper.UnitNameOffset = nameOffset; end
```
"Enable" is the unit plopper enable block from 7.3.

### 7.5 Selected City (state `CityView`)

Every entry first resolves the city this way:
```lua
local pCity = nil;
local pPlayer = Players[g_TunerSelectedCityPlayerID];
if pPlayer ~= nil then pCity = pPlayer:GetCityByID(g_TunerSelectedCityID); end
if pCity == nil then pCity = UI:GetHeadSelectedCity(); end
```

| Operation | Lua (after resolving `pCity`) |
|---|---|
| Increase / Decrease Pop | `if pCity ~= nil then pCity:ChangePopulation(1, true); end` / `(-1, true)` |
| City Population field | `return pCity:GetPopulation();` / `pCity:SetPopulation(value, true);` |
| City Food field | `return pCity:GetFood();` / `pCity:SetFood(value);` |
| WLTKD Turns field | `return pCity:GetWeLoveTheKingDayCounter();` / `pCity:SetWeLoveTheKingDayCounter(value);` |
| Damage field | `return pCity:GetDamage();` / `pCity:SetDamage(value);` |
| Gold Yield Modifier (read-only) | `return pCity:GetYieldRateModifier(2);` |
| AI Focus: None / Food / Production / Gold / Great People | `Network.SendSetCityAIFocus(pCity :GetID(), CityAIFocusTypes.NO_CITY_AI_FOCUS_TYPE);` (or `CITY_AI_FOCUS_TYPE_FOOD`, `_PRODUCTION`, `_GOLD`, `_GREAT_PEOPLE`) |
| Create Apollo Program | `if (pCity ~= nil ) then pCity:CreateApolloProgram() end` |
| Buildings multiselect: tick / untick | `local building = GameInfo.Buildings["BUILDING_" .. tostring(selection)]; if(building ~= nil) then pCity:SetNumRealBuilding(building.ID, 1); end` (untick: `0`). These use `UI:GetHeadSelectedCity()` only. |
| City list selection | Sets `g_TunerSelectedCityPlayerID` and `g_TunerSelectedCityID` from the `"playerID,cityID;..."` row |

VP: `SetNumRealBuilding(id, n[, bNoBonus=true])`; `ChangePopulation` / `SetPopulation(value[, bReassignPop=true])`.

### 7.6 Selected Unit (state `DebugMenu`)

| Operation | Lua |
|---|---|
| Damage field | `return UI.GetHeadSelectedUnit():GetDamage();` / `UI.GetHeadSelectedUnit():SetDamage(value)` |
| Change Experience field | `return UI.GetHeadSelectedUnit():GetExperience();` / `UI.GetHeadSelectedUnit():SetExperience(value)` |
| Embark | `local unit = UI.GetHeadSelectedUnit(); unit:Embark()` (VP: `Embark([plot])`, returns bool) |
| Set Unit Action (animation) | `local unit = UI.GetHeadSelectedUnit(); local player = Players[Game.GetActivePlayer()]; SetUnitActionCodeDebug(player:GetID(), unit:GetID(), g_UnitAction);` with codes IDLE=1000, ATTACK=1100, DEATH=1200, RUN=1400, FORTIFY=1500, WORK=1900, and others |

### 7.7 Options (state `WorldView`)

| Operation | Lua |
|---|---|
| Max Turns | `return Game.GetMaxTurns()` / `local iTurn = Game.GetElapsedGameTurns() if value > iTurn then Game.SetMaxTurns(value) end` |
| Game option toggles | `return Game.IsOption(GameOptionTypes.GAMEOPTION_X)` / `Game.SetOption(GameOptionTypes.GAMEOPTION_X, value)` for X = POLICY_SAVING, PROMOTION_SAVING, COMPLETE_KILLS, DISABLE_START_BIAS, END_TURN_TIMER_ENABLED, NEW_RANDOM_SEED, NO_GOODY_HUTS, NO_BARBARIANS, NO_CITY_RAZING, ONE_CITY_CHALLENGE, QUICK_COMBAT, RAGING_BARBARIANS, RANDOM_PERSONALITIES, ALWAYS_PEACE, ALWAYS_WAR, NO_CHANGING_WAR_PEACE, LOCK_MODS |
| Victory toggles | `return PreGame.IsVictory(GameInfo.Victories.VICTORY_X.ID)` / `PreGame.SetVictory(GameInfo.Victories.VICTORY_X.ID, value)` for CULTURAL, DIPLOMATIC, DOMINATION, SPACE_RACE, TIME |

### 7.8 Read-only inspectors

| Panel (state) | Lua |
|---|---|
| Players (`WorldView`) | For each `i < GameDefines.MAX_CIV_PLAYERS`: `tostring(i)`, `pPlayer:GetTeam()`, `IsAlive()`, `IsTurnActive()`, and when `IsEverAlive()`: `GetCivilizationShortDescription()`, `GetNickName()`, `PreGame.GetHandicap(i)` |
| Game UI (`WorldView`) | `for iIndex = 0, UI.GetPopupTypeCount()-1 do local hType = UI.GetPopupTypeByIndex(iIndex); if UI.IsPopupTypeOpen(hType) then ... UI.GetPopupTypeName(hType) end end` |
| File System (`OptionsMenu`) | `bValid, szRow = Profiler.GetEngineInfoRowString("FileSystem", "SearchPaths", iCount)` in a loop |
| Selected City, "Revealed to Civilization" list (`CityView`) | `pCity:IsRevealed(i)` per player |
| Network (`MainMenu`, `InGame`) | `Network.IsPlayerConnected(i)`, `PreGame.GetNickName(i)`, `Matchmaking.GetHostID()`. Actions: `Network.CloseConnection(g_networkConnectionSelected);`, `Network.ForceResync();`, `Network.QueueForceResync();` |

### 7.9 Debug and memory (state `Main State`)

These are relevant to the memory investigation.

| Operation | Lua |
|---|---|
| Collect Garbage | `collectgarbage("collect");` |
| Lua memory in use (KB) | `return collectgarbage("count");` |
| Collect SQL / Lua / System / DLL-context memory usage | `DB.CollectMemoryUsage();` / `CollectLuaMemoryUsage();` / `CollectSystemMemoryUsage();` / `CollectDllContextMemoryUsage();` |
| Read those allocators | `local m = DB.GetMemoryUsage()` (likewise `GetLuaMemoryUsage()`, `GetSystemMemoryUsage()`, `GetDllContextMemoryUsage()`). Fields used: `CurrentAllocatedBytes, CurrentOverheadBytes, CurrentReservedBytes, CurrentUncommitedBytes` (sic), `NumRegions, MaxUserAllocatedBytes, CurrentUserAllocatedBytes`. The panel shows Total Allocated = Allocated + Overhead and Total Heap = Uncommitted + Reserved. |
| SQL statement count / list | `return DB.StatementCount();` / `for i = 0, DB.StatementCount() - 1 do table.insert(listItems, DB.StatementSQL(i)) end` |
| Lua Mem Tracking panel | `BeginLuaMemTracking()`, `EndLuaMemTracking()`, `LuaMemTrackingResults = GetLuaMemTrackingResults()` |
| Audio Logging panel | `BeginAudioLogging()`, `EndAudioLogging()`, `AudioLog = GetAudioLog()` (fields `TriggeredSounds[i].Sound/Time/Volume/Pan/Filtered/Denied/Unit/UnitMember`) |

## 8. Engine-only functions: are they in the shipping binary?

A byte search for each name in `CivilizationV_DX11.exe` and the DLLs in the game root, done 2026-09-16. Finding a name
suggests the function is registered, but none of these was called (UNTESTED).

| Found in `CivilizationV_DX11.exe` | Not found anywhere |
|---|---|
| `GetLuaMemoryUsage`, `CollectLuaMemoryUsage`, `GetSystemMemoryUsage`, `CollectSystemMemoryUsage`, `GetDllContextMemoryUsage`, `CollectDllContextMemoryUsage`, `GetMemoryUsage`/`CollectMemoryUsage`/`StatementCount`/`StatementSQL` (also in `CvGameDatabaseWin32Final Release.dll`), `GetEngineInfoRowString`, `SetUnitActionCodeDebug`, `TunerSetUnitState`, `GetUnitSimHandle`, `ToggleStrategicView`, `GetStrategicViewOverlays`, `BeginAudioLogging`, `GetAudioLog`, `GetCurCivSong`, `GetPopupTypeCount`, `IsPopupTypeOpen`, `SetInterfaceModeDebugItemID1`, `StateMachineRequestStates`, `AudioDebugChangeMusic`, `ForceResync`, `QueueForceResync`, `CloseConnection`, `SetVolumeKnobValue` | `BeginLuaMemTracking`, `EndLuaMemTracking`, `GetLuaMemTrackingResults` (so the Lua Mem Tracking panel needs a memory-tracker build), `ToggleDebugSound`, `SetTimeSpeedModifier` |

The DX11 exe's own config comments also mention `SendRemarksToTuner` and "Disable sounds from gamecore (also can set via tuner)".

## 9. What this exploration did to the live FireTuner (for the record)

1. Read the UIA tree and saved screenshots. No changes.
2. Selected tab `* New Panel *` through UIA. That opened the modal **Panel Builder** and blocked the UIA call. Closed it with `BM_CLICK` on `btnCancel` (nothing created), then re-selected `Lua Console`. The screenshot matches the starting state.
3. Set the input to `print(1)` through ValuePattern and posted Enter. FireTuner showed "Can't run a command while disconnected". Dismissed it (OK), cleared the input to `""`. The output is unchanged (`Working late?`).
4. Side effect: the message box activated FireTuner, so it may have taken the foreground from the user's window. No settings, panels or files were touched; `PanelConfig.xml` and `UserSettings.xml` were only read.

2026-09-17 (connected):
5. Selected tab `Lua Console` through UIA (this took the foreground from the user's Claude window), read status,
   states and output, switched the console state `Main State` -> `InGame` -> `Main State` by combo messages, ran the
   lines in section 5a, then re-selected the `Map` tab it started on. The console state was left on `Main State`, as
   found. About 25 lines of test output (`@@ft-done-*` sentinels, `317`, the `boom` error) were added to the console
   output and to `Lua.log`. Nothing else was changed; `Clear Output` was not pressed.
