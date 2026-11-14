#!/usr/bin/env python3
"""fake_dll_responder.py - emulates the VP DLL's side of the snapshot and Lua-exec channels.

For testing myth_watch.py without the game. Mirrors MemoryDiagnostics.cpp PollSnapshotRequest:
  * creates (or opens) the auto-reset events Local\\VPMemSnapshot and Local\\VPMemSnapshotDone
  * on each request, reads the label from <cache>/memsnap_request.txt keeping printable ASCII up to
    the first CR/LF (95 chars max, "unlabelled" if empty or missing), then DELETES the file as the
    current DLL does (--keep-request for the old behaviour)
  * "samples" for --delay-s seconds, writes one tab-separated Key=Value line ending in \\n to
    <cache>/memsnap_done.txt, then sets the done event
Fault injection: --wrong-label-at N answers request N with a wrong Label; --stale-seq-at N answers it
re-using the previous SnapSeq (both repeatable; N counts snapshot requests from 1).

With --lua it also mirrors CvLuaSupport.cpp PollExternalLuaRequest: on Local\\VPLuaExec it reads and
deletes <cache>/luaexec_request.lua, takes the --@id / --@state header lines, and writes
luaexec_result.txt ("id=..\\tstate=..\\tms=..\\tturn=.." then "ok=1\\tnret=2", two values and a print
section) before setting Local\\VPLuaExecDone. A chunk containing "error(" gets an ok=0 reply.
--lua-silent-at N reads and deletes Lua request N but never answers it (the "picked up, still
running" timeout path).

Exits by itself after --lifetime-s so a forgotten background copy cannot linger.

  python fake_dll_responder.py --cache-dir selftest_cache [--delay-s 0.8] [--slow-first-s 2.5]
         [--wrong-label-at 2] [--stale-seq-at 4] [--lua] [--lua-silent-at 2] [--lifetime-s 900]
"""

import argparse
import os
import random
import sys
import time

import win32api
import win32event

REQUEST_EVENT = 'Local\\VPMemSnapshot'
DONE_EVENT = 'Local\\VPMemSnapshotDone'
LUA_REQUEST_EVENT = 'Local\\VPLuaExec'
LUA_DONE_EVENT = 'Local\\VPLuaExecDone'
LABEL_CHARS = 96


def read_label(path, keep):
    try:
        with open(path, 'rb') as f:
            raw = f.read(255)
    except OSError:
        return 'unlabelled'
    if not keep:
        try:
            os.remove(path)
        except OSError:
            pass
    clean = []
    for byte in raw:
        if byte in (0x0D, 0x0A) or len(clean) >= LABEL_CHARS - 1:
            break
        if 0x20 <= byte <= 0x7E:
            clean.append(chr(byte))
    return ''.join(clean) or 'unlabelled'


def read_lua_request(path):
    """(id, state, code) or (None, None, None) when there is no readable request."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return None, None, None
    try:
        os.remove(path)
    except OSError:
        pass
    if data.startswith(b'\xef\xbb\xbf'):
        data = data[3:]
    text = data.decode('utf-8', 'replace')
    request_id, state = '', ''
    lines = text.split('\n')
    for line in lines:
        if not line.startswith('--@'):
            break
        header = line[3:].rstrip('\r')
        if header.startswith('id='):
            request_id = header[3:]
        elif header.startswith('state='):
            state = header[6:]
    return request_id, state, text


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cache-dir', required=True)
    parser.add_argument('--delay-s', type=float, default=0.8, help='emulated sampling time')
    parser.add_argument('--slow-first-s', type=float, default=None,
                        help='sampling time of the first request only (to provoke a late reply)')
    parser.add_argument('--wrong-label-at', type=int, action='append', default=[],
                        help='answer snapshot request N with a wrong Label (repeatable)')
    parser.add_argument('--stale-seq-at', type=int, action='append', default=[],
                        help='answer snapshot request N re-using the previous SnapSeq (repeatable)')
    parser.add_argument('--keep-request', action='store_true',
                        help='do not delete memsnap_request.txt after reading it (pre-2026-09-16 DLL)')
    parser.add_argument('--lua', action='store_true', help='also answer the Lua-exec channel')
    parser.add_argument('--lua-delay-s', type=float, default=0.2, help='emulated Lua run time')
    parser.add_argument('--lua-silent-at', type=int, action='append', default=[],
                        help='read Lua request N but never answer it (repeatable)')
    parser.add_argument('--lifetime-s', type=float, default=900.0, help='exit after this long')
    parser.add_argument('--turn', type=int, default=66)
    args = parser.parse_args()

    os.makedirs(args.cache_dir, exist_ok=True)
    request = win32event.CreateEvent(None, False, False, REQUEST_EVENT)
    done = win32event.CreateEvent(None, False, False, DONE_EVENT)
    handles = [request]
    if args.lua:
        lua_request = win32event.CreateEvent(None, False, False, LUA_REQUEST_EVENT)
        lua_done = win32event.CreateEvent(None, False, False, LUA_DONE_EVENT)
        handles.append(lua_request)
    request_path = os.path.join(args.cache_dir, 'memsnap_request.txt')
    done_path = os.path.join(args.cache_dir, 'memsnap_done.txt')
    lua_request_path = os.path.join(args.cache_dir, 'luaexec_request.lua')
    lua_result_path = os.path.join(args.cache_dir, 'luaexec_result.txt')
    print('responder pid %d ready, cache %s, lua %s' % (os.getpid(), os.path.abspath(args.cache_dir), args.lua),
          flush=True)

    requests = 0            # snapshot requests seen
    seq = 0                 # last SnapSeq written
    lua_requests = 0
    committed_kb = 2_400_000
    end = time.monotonic() + args.lifetime_s
    while time.monotonic() < end:
        rc = win32event.WaitForMultipleObjects(handles, False, 250)
        if rc == win32event.WAIT_TIMEOUT:
            continue
        which = rc - win32event.WAIT_OBJECT_0

        if which == 0:
            requests += 1
            label = read_label(request_path, args.keep_request)
            start = win32api.GetTickCount()
            time.sleep(args.slow_first_s if (requests == 1 and args.slow_first_s is not None) else args.delay_s)
            sample_ms = (win32api.GetTickCount() - start) & 0xFFFFFFFF
            committed_kb += random.randint(-2048, 8192)
            injected = []
            reply_label = label
            if requests in args.wrong_label_at:
                reply_label = label + '-WRONG'
                injected.append('wrong label')
            if requests in args.stale_seq_at and seq > 0:
                reply_seq = seq                              # the DLL never does this; a stale file would
                injected.append('stale seq')
            else:
                seq += 1
                reply_seq = seq
            line = ('SnapSeq=%d\tLabel=%s\tTurn=%d\tLogger=1\tHeaps=1\tCensus=1\tSampleMs=%u\tWalkMs=%u'
                    '\tWriteMs=%u\tCommittedKB=%d\tLargestFreeKB=%d\tLargestFreeLowKB=%d\tBusyKB=%d'
                    '\tHookLiveKB=%d\tLuaKB=%d\n') % (
                reply_seq, reply_label, args.turn, sample_ms, sample_ms // 2, 12, committed_kb, 900_000, 41_000,
                1_700_000, 85_000, 92_000)
            with open(done_path, 'wb') as f:
                f.write(line.encode('ascii'))
            win32event.SetEvent(done)
            print('answered snapshot request %d (label %s) with SnapSeq=%d Label=%s%s' % (
                requests, label, reply_seq, reply_label, (' [%s]' % ', '.join(injected)) if injected else ''),
                flush=True)

        elif which == 1:
            lua_requests += 1
            request_id, state, code = read_lua_request(lua_request_path)
            if lua_requests in args.lua_silent_at:
                print('lua request %d (id %s) read and deliberately left unanswered' % (lua_requests, request_id),
                      flush=True)
                continue
            start = win32api.GetTickCount()
            time.sleep(args.lua_delay_s)
            ms = (win32api.GetTickCount() - start) & 0xFFFFFFFF
            state = state or 'Main'
            if code is None:
                body = 'ok=0\n--- error ---\nno readable luaexec_request.lua in the cache folder\n'
            elif 'error(' in code:
                body = 'ok=0\n--- error ---\nluaexec:3: fake error\nstack traceback:\n\t[C]: in function \'error\'\n'
            else:
                body = ('ok=1\tnret=2\n[1] %d\n[2] "fake responder, state %s"\n--- print ---\nran %d bytes\n'
                        % (args.turn, state, len(code.encode('utf-8'))))
            text = 'id=%s\tstate=%s\tms=%d\tturn=%d\n%s' % (request_id or '', state, ms, args.turn, body)
            with open(lua_result_path, 'wb') as f:
                f.write(text.encode('utf-8'))
            win32event.SetEvent(lua_done)
            print('answered lua request %d (id %s, state %s)' % (lua_requests, request_id, state), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
