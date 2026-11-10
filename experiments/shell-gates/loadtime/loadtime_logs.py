#!/usr/bin/env python3
"""Split a Civ 5 launch-and-load into phases from the game's own Logs folder (read-only).

Usage:  python loadtime_logs.py <Logs dir or a dir containing Logs/> [more ...]
        python loadtime_logs.py --table <dir> [more ...]     one row per run

All timestamps in the Logs are the same clock (seconds, ~uptime). Anchors used:
  CRT start      = time of an EUI print minus the os.clock() value it prints (MSVC clock() = wall time
                   since process start; checked 2026-11-10 against the launcher's spawn time: 0.07 s apart)
  APP.log        "Command Line Args" (first line the EXE writes)
  stopwatch.log  EXE + DLL cvStopWatch scopes, line written at scope END: [end] , name, duration
  system_init.log  SystemInit records are posted into the 4 MB g_EventSystemLocalMachine queue
                   (alloc 0x5CA5E0, type 0x66) and WRITTEN WHEN THAT QUEUE IS PUMPED (0x459D20), so a line's
                   timestamp is the next pump, not the end of the timed scope. A long gap between two flushes
                   = a long time without a 4 MB-queue pump on the main thread.
  init_host.log  "Multiplayer Init(Host)" (load started on the network layer)
  net_message_debug.log  NetLoadGame SEND (EXE has read the save) / RECV (DLL deserialises) /
                   NetPlayerRegisterComplete RECV (DLL read done)
  Lua.log        "vp-dll-dev: loading" (vp_modgame's MainMenu issues the load), first "LoadScreen:" line
"""
import os, re, sys

TS = re.compile(r'^\[(\d+\.\d+)\]')


def _lines(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def _ts(l):
    m = TS.match(l)
    return float(m.group(1)) if m else None


def analyse(d):
    L = os.path.join(d, 'Logs') if os.path.isdir(os.path.join(d, 'Logs')) else d
    r = {'dir': d}
    for l in _lines(os.path.join(L, 'APP.log')) + _lines(os.path.join(L, 'init_host.log')):
        if 'Command Line Args' in l: r['app'] = _ts(l)
        if 'Multiplayer Init(Host)' in l: r['mpinit'] = _ts(l)
    acts = []
    for l in _lines(os.path.join(L, 'stopwatch.log')):
        t = _ts(l)
        if t is None: continue
        parts = l.split(', ')
        try: dur = float(parts[-1])
        except ValueError: continue
        name = parts[1].strip() if len(parts) > 2 else ''
        r.setdefault('sw_first', t - dur)
        if name == 'SetActiveDLCandMods': acts.append((t - dur, t, dur))
        if name == 'ActivateEnabledMods': r['act_mods'] = (t - dur, t, dur)
        if name == 'LoadUnitLibraries via Database': r.setdefault('lul', []).append((t - dur, dur))
        if name == 'Validate FK Constraints': r.setdefault('fk', []).append(dur)
        if name.startswith('SetActiveDLCandMods - Cache Game Database Data'): r.setdefault('cache', []).append(dur)
        if name == 'Reload Modded Audio': r['audio_start'] = t - dur
    r['acts'] = acts
    prev = None
    for l in _lines(os.path.join(L, 'Lua.log')):
        t = _ts(l)
        if t is None: continue
        if 'vp-dll-dev: loading' in l: r['lua_load'] = t
        if 'LoadScreen:' in l and 'lua_loadscreen' not in r: r['lua_loadscreen'] = t
        if 'EUI_context: EUI CONTEXT' in l: r['lua_ctx'] = t
        m = re.search(r'Loading EUI plot help\s+LuaContext: \w+\s+([\d.]+)', l)
        if m: r['crt_start'] = t - float(m.group(1))
        if prev is not None and t - prev > 5 and 'lua_ctx' in r and 'lua_silence' not in r:
            r['lua_silence'] = (prev, t)
        prev = t
    for l in _lines(os.path.join(L, 'net_message_debug.log')):
        if 'Message chunk' in l: continue
        t = _ts(l)
        if 'NetLoadGame' in l and 'SEND' in l: r['netload_send'] = t
        if 'NetLoadGame' in l and 'RECV' in l: r['netload_recv'] = t
        if 'NetPlayerRegisterComplete' in l and 'RECV' in l: r['regcomplete'] = t
    r['si'] = []
    for l in _lines(os.path.join(L, 'system_init.log')):
        m = re.match(r'^\[(\d+\.\d+)\] DBG: SystemInit: (.*): (\d+) Milliseconds', l)
        if m: r['si'].append((float(m.group(1)), m.group(2), int(m.group(3))))
    return r


def si(r, name):
    for x in r['si']:
        if x[1] == name: return x
    return None


COLS = ['crt>app', 'app>sw', 'sw>act1', 'act1', 'sysStart', 'act2', 'cache2', 'fk_all', 'unitLM_rl',
        'load>mp', 'mp>netld', 'dllread', '>ldscr', 'terrain', 'luactx', 'GAP', 'tail', 'gpu>rdy', 'wfnp', 'TOTAL']


def phases(r):
    """Return the 20 phase durations (seconds) in COLS order; None where an anchor is missing."""
    def d(a, b):
        return None if a is None or b is None else b - a
    a1 = r['acts'][0] if r['acts'] else (None, None, None)
    am = r.get('act_mods', (None, None, None))
    lul = r.get('lul', [])
    first_si = r['si'][0][0] if r['si'] else None
    ti, lc = si(r, 'StartInitTerrainFromGameplayMap()'), si(r, 'GameViewStateLuaContextInit::StartupGame()')
    sg, tsh = si(r, 'LoadGameState::GameViewState::StartupGame()'), si(r, 'CreateTreeShadows()')
    ia, wf = si(r, 'NetPlayer::IAmReady()'), si(r, 'WaitForNetPlayers()')
    g = lambda x, i: x[i] if x else None
    return [
        d(r.get('crt_start'), r.get('app')), d(r.get('app'), r.get('sw_first')), d(r.get('sw_first'), a1[0]), a1[2],
        d(a1[1], first_si), am[2], (r.get('cache') or [None])[-1], sum(r.get('fk', [])) or None,
        d(lul[-1][0], r.get('audio_start')) if lul and am[0] and lul[-1][0] > am[0] else None,
        d(r.get('lua_load'), r.get('mpinit')), d(r.get('mpinit'), r.get('netload_send')),
        d(r.get('netload_recv'), r.get('regcomplete')), d(r.get('regcomplete'), r.get('lua_loadscreen')),
        (ti[2] / 1000.0) if ti else None, d(r.get('lua_ctx'), g(lc, 0)), d(g(lc, 0), g(sg, 0)), d(g(sg, 0), g(tsh, 0)),
        d(g(tsh, 0), g(ia, 0)), d(g(ia, 0), g(wf, 0)), d(r.get('crt_start'), g(wf, 0))]


def table(dirs):
    print('run'.ljust(34) + ' '.join(c.rjust(9) for c in COLS))
    for dd in dirs:
        v = phases(analyse(dd))
        name = os.path.basename(os.path.normpath(dd))[:33]
        print(name.ljust(34) + ' '.join(('%9.2f' % x) if x is not None else '      n/a' for x in v))


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--table':
        table(args[1:])
    else:
        for dd in args:
            r = analyse(dd)
            print('###', dd)
            for c, v in zip(COLS, phases(r)):
                print('  %-10s %s' % (c, 'n/a' if v is None else '%.2f s' % v))
            if 'lua_silence' in r:
                print('  Lua.log silence after the UI contexts: %.2f s' % (r['lua_silence'][1] - r['lua_silence'][0]))
            for t, n, ms in r['si']:
                print('  si %.3f %-50s %6d ms' % (t, n, ms))
