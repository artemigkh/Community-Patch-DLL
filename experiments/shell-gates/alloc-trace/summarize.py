"""Summarise crashlogs/allocTrace.log written by EngineAllocTrace (see README.md next to this file).

    python summarize.py <allocTrace.log> [--session N] [--top 25] [--all-sessions]

The log is appended across game launches; each launch starts with a "# ... EngineAllocTrace active" header.
By default the LAST session is summarised; --session 1 is the first, --all-sessions does each in turn.

Sections:
  sites         allocations grouped by allocator site: file:line when the engine allocator gave one, else the
                calling EXE address. Count, bytes allocated, live count and bytes at the end of the log, largest.
  callers       the same grouped by the calling EXE address only (several sites can share a file:line).
  largest live  the biggest blocks still allocated at the end of the log, with when (turn) and who.
  chains        reallocation chains: a block freed right after a larger one was allocated by the same code on
                the same thread (an FFastVector / std::vector growing), plus realloc() moves.
  2:1 pairs     live blocks allocated back to back whose sizes are exactly 2:1 - the signature of the
                0x26-filled / 0x25-filled block pair this instrument was built to name.
  failures      requests that returned NULL (any size is logged when it fails).

Addresses are printed as EXE RVAs and as VAs at the preferred ImageBase 0x400000, which is what the
disassembly (llvm-objdump of CivilizationV_DX11.exe) shows.
"""
import argparse
import collections
import re
import sys

IMAGE_BASE = 0x400000
LINE_RE = re.compile(r'(\w+)=(\S+)')


def parse_sessions(path):
    sessions = []
    current = None
    with open(path, 'r', encoding='latin-1') as handle:
        for raw in handle:
            line = raw.rstrip('\r\n')
            if not line:
                continue
            if line.startswith('#'):
                if 'EngineAllocTrace' in line:
                    current = {'header': line, 'events': [], 'turns': []}
                    sessions.append(current)
                elif current is not None:
                    current.setdefault('notes', []).append(line)
                continue
            if current is None:
                current = {'header': '(no header)', 'events': [], 'turns': []}
                sessions.append(current)
            kind = line[0]
            fields = dict(LINE_RE.findall(line))
            fields['kind'] = kind
            fields['raw'] = line
            for key in ('seq', 'tick', 'tid', 'size', 'line', 'age', 'oldseq', 'oldsize', 'turn', 'count', 'align'):
                if key in fields:
                    try:
                        fields[key] = int(fields[key])
                    except ValueError:
                        pass
            for key in ('ptr', 'old'):
                if key in fields:
                    fields[key] = int(fields[key], 16)
            fields['fail'] = fields.get('fail') == '1'
            if kind == 'T':
                current['turns'].append(fields)
            current['events'].append(fields)
    return sessions


def rva(text):
    """'EXE+0x1b73ed' -> 0x1b73ed; a bare address -> None."""
    if text and text.startswith('EXE+0x'):
        return int(text[6:], 16)
    return None


def fmt_code(text):
    value = rva(text)
    if value is None:
        return text or '-'
    return f'EXE+0x{value:x} (VA 0x{value + IMAGE_BASE:x})'


def site_of(event):
    if 'file' in event:
        return f"{event['file']}:{event.get('line', '?')}"
    return f"{event.get('api', '?')} from {event.get('caller', '?')}"


def mb(value):
    return f'{value / 1048576:9.2f} MB'


def summarise(session, top):
    events = session['events']
    print('=' * 100)
    print(session['header'])
    allocs = {}           # seq -> event (A or R that produced a block)
    live = {}             # ptr -> seq
    last_turn = None
    turn_at_seq = {}
    by_tid = collections.defaultdict(list)
    links = {}            # old seq -> new seq
    link_kind = {}
    reverse_targets = set()
    failures = []

    for event in events:
        kind = event['kind']
        if kind == 'T':
            last_turn = event.get('turn')
            continue
        seq = event.get('seq')
        if kind == 'A':
            if event['fail']:
                failures.append(event)
                continue
            event['turn_at'] = last_turn
            allocs[seq] = event
            if event.get('untracked') != '1':
                live[event['ptr']] = seq
            by_tid[event.get('tid')].append(event)
        elif kind == 'R':
            if event['fail']:
                failures.append(event)
                continue
            event['turn_at'] = last_turn
            old_seq = event.get('oldseq')
            if old_seq is not None:
                live.pop(event.get('old'), None)
                links[old_seq] = seq
                link_kind[old_seq] = 'realloc'
                reverse_targets.add(seq)
            if event.get('ptr') and event.get('size', 0) > 0:
                allocs[seq] = event
                live[event['ptr']] = seq
            by_tid[event.get('tid')].append(event)
        elif kind == 'F':
            live.pop(event.get('ptr'), None)
            freed = allocs.get(seq)
            if freed is not None:
                freed['freed'] = event
            # Growth: the block this thread allocated just before was a bigger one from the same allocator and
            # the same site (or nearby code), and nothing else has claimed it - FFastVector::Reserve and
            # std::vector growth allocate the new buffer, copy, then free the old one.
            caller = rva(event.get('caller'))
            recent = by_tid.get(event.get('tid'), [])
            candidate = recent[-1] if recent else None
            if freed is not None and candidate is not None and candidate['seq'] > seq                     and candidate['seq'] not in reverse_targets and candidate.get('size', 0) > event.get('size', 0)                     and candidate.get('api') == freed.get('api')                     and 0 <= event.get('tick', 0) - candidate.get('tick', 0) <= 2000:
                cand_caller = rva(candidate.get('caller'))
                if 'file' in candidate and 'file' in freed:
                    same_site = site_of(candidate) == site_of(freed)
                else:
                    alloc_caller = rva(freed.get('caller'))
                    same_site = cand_caller is not None and alloc_caller is not None and abs(cand_caller - alloc_caller) < 0x800
                if same_site:
                    links[seq] = candidate['seq']
                    link_kind[seq] = 'grow'
                    reverse_targets.add(candidate['seq'])

    turns = session['turns']
    if turns:
        print(f"turns {turns[0].get('turn')}..{turns[-1].get('turn')}; at the last marker {turns[-1].get('tracked')} blocks "
              f"/ {mb(int(turns[-1].get('trackedbytes', 0)))} tracked")
    live_events = [allocs[s] for s in live.values() if s in allocs]
    print(f"{len(allocs)} large blocks allocated, {len(live_events)} still live ({mb(sum(e['size'] for e in live_events)).strip()}), "
          f"{len(failures)} failed requests")

    # Sites
    sites = collections.OrderedDict()
    for event in allocs.values():
        entry = sites.setdefault(site_of(event), {'n': 0, 'bytes': 0, 'live_n': 0, 'live_bytes': 0, 'max': 0,
                                                  'callers': collections.Counter(), 'api': event.get('api'), 'tag': event.get('tag')})
        entry['n'] += 1
        entry['bytes'] += event['size']
        entry['max'] = max(entry['max'], event['size'])
        entry['callers'][event.get('caller')] += 1
        if event.get('ptr') in live and live[event['ptr']] == event['seq']:
            entry['live_n'] += 1
            entry['live_bytes'] += event['size']
    print(f'\n-- sites, by live bytes (top {top})')
    print(f"{'live':>12} {'n live':>6} {'allocated':>12} {'n':>5} {'largest':>12}  site [api tag] callers")
    for name, entry in sorted(sites.items(), key=lambda kv: (-kv[1]['live_bytes'], -kv[1]['bytes']))[:top]:
        callers = ', '.join(fmt_code(c) for c, _ in entry['callers'].most_common(2))
        print(f"{mb(entry['live_bytes'])} {entry['live_n']:6d} {mb(entry['bytes'])} {entry['n']:5d} {mb(entry['max'])}  "
              f"{name} [{entry['api']} {entry['tag'] or ''}] {callers}")

    # Callers
    callers = collections.defaultdict(lambda: {'n': 0, 'bytes': 0, 'live_bytes': 0, 'max': 0, 'sites': set()})
    for event in allocs.values():
        entry = callers[event.get('caller')]
        entry['n'] += 1
        entry['bytes'] += event['size']
        entry['max'] = max(entry['max'], event['size'])
        entry['sites'].add(site_of(event))
        if event.get('ptr') in live and live[event['ptr']] == event['seq']:
            entry['live_bytes'] += event['size']
    print(f'\n-- callers, by live bytes (top {top})')
    for caller, entry in sorted(callers.items(), key=lambda kv: (-kv[1]['live_bytes'], -kv[1]['bytes']))[:top]:
        print(f"{mb(entry['live_bytes'])} live {mb(entry['bytes'])} allocated in {entry['n']:5d}, largest {mb(entry['max'])}  "
              f"{fmt_code(caller)}  {'; '.join(sorted(entry['sites']))[:120]}")

    # Largest live
    print(f'\n-- largest live blocks (top {top})')
    for event in sorted(live_events, key=lambda e: -e['size'])[:top]:
        print(f"{mb(event['size'])}  seq {event['seq']:6d}  turn {event.get('turn_at')}  tid {event.get('tid')}  "
              f"{event.get('api')} {site_of(event)}  ptr 0x{event['ptr']:08x}")
        print(f"             caller {fmt_code(event.get('caller'))}  stack {event.get('stack', '-')[:160]}")

    # Chains
    reverse = {new: old for old, new in links.items()}
    chains = []
    for start in links:
        if start in reverse:
            continue
        chain = [start]
        while chain[-1] in links:
            chain.append(links[chain[-1]])
        chains.append(chain)
    chains.sort(key=lambda c: -max(allocs[s]['size'] for s in c if s in allocs))
    print(f'\n-- growth / realloc chains (top {top}, longest block first)')
    if not chains:
        print('   none')
    for chain in chains[:top]:
        sizes = [allocs[s]['size'] for s in chain if s in allocs]
        first = allocs.get(chain[0], {})
        last = allocs.get(chain[-1], {})
        state = 'live' if last.get('ptr') in live else 'freed'
        print(f"   {len(chain)} blocks, {' -> '.join(f'{s / 1048576:.2f}' for s in sizes)} MB ({state}), "
              f"turn {first.get('turn_at')}..{last.get('turn_at')}  {link_kind.get(chain[0])}  {site_of(first)}  "
              f"{fmt_code(first.get('caller'))}")

    # 2:1 pairs
    print('\n-- live 2:1 size pairs allocated back to back on one thread (the 0x26 / 0x25 signature)')
    ordered = sorted(live_events, key=lambda e: e['seq'])
    found = 0
    for i, first in enumerate(ordered):
        for second in ordered[i + 1:i + 4]:
            if second.get('tid') != first.get('tid'):
                continue
            big, small = (first, second) if first['size'] >= second['size'] else (second, first)
            if small['size'] * 2 == big['size']:
                found += 1
                print(f"   {mb(big['size'])} + {mb(small['size'])}  seq {first['seq']}/{second['seq']} turn {first.get('turn_at')}  "
                      f"{site_of(big)} | {site_of(small)}  {fmt_code(big.get('caller'))}")
    if not found:
        print('   none')

    print('\n-- failed requests')
    if not failures:
        print('   none')
    for event in failures:
        print(f"   seq {event.get('seq')} tick {event.get('tick')} {event.get('api')} size {event.get('size')} "
              f"{site_of(event)} caller {fmt_code(event.get('caller'))}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log')
    parser.add_argument('--session', type=int, default=0, help='1-based session; default: the last')
    parser.add_argument('--all-sessions', action='store_true')
    parser.add_argument('--top', type=int, default=25)
    args = parser.parse_args()

    sessions = parse_sessions(args.log)
    if not sessions:
        print('no sessions in the log')
        return 1
    print(f'{len(sessions)} session(s) in {args.log}')
    if args.all_sessions:
        chosen = sessions
    elif args.session:
        chosen = [sessions[args.session - 1]]
    else:
        chosen = [sessions[-1]]
    for session in chosen:
        summarise(session, args.top)
    return 0


if __name__ == '__main__':
    sys.exit(main())
