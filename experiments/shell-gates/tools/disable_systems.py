"""Set [DisableSystems] keys in the game's config.ini for one run, and restore it verified by hash.

    python disable_systems.py backup                 copy config.ini to the backup folder (once; refuses to
                                                     overwrite a backup whose hash differs from the live file
                                                     unless the live file already equals it)
    python disable_systems.py set B4                 a named preset (see PRESETS), or
    python disable_systems.py set FluidFOW Particle  explicit keys set to 1 (all other keys keep their value)
    python disable_systems.py restore                copy the backup back and verify the SHA-256
    python disable_systems.py show                   current [DisableSystems] values and whether config.ini
                                                     equals the backup

Backup folder: %USERPROFILE%\\Documents\\shell-gates-backup (config.ini.orig + SHA256.txt).
"""
import hashlib
import re
import shutil
import sys
from pathlib import Path

CFG = Path.home() / "Documents/My Games/Sid Meier's Civilization 5/config.ini"
BK = Path.home() / 'Documents/shell-gates-backup'
ORIG = BK / 'config.ini.orig'

A4 = ['Decal', 'City', 'Forest', 'FOW', 'FluidFOW', 'DistanceFog', 'Waves', 'YieldIcons', 'MovementPath',
      'Particle', 'Combat', 'Projectile']
PRESETS = {
    'A4': A4,
    'B4': A4 + ['Overlay', 'OverlayDebug', 'StrategicView', 'Minimap', 'ColorKeyEdit'],
    'A5': [k for k in A4 if k != 'FluidFOW'],
    'FluidFOW': ['FluidFOW'],
    'none': [],
}


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def set_keys(keys):
    text = CFG.read_bytes().decode('latin1')
    m = re.search(r'(?ms)^\[DisableSystems\]\s*$(.*?)(?=^\[|\Z)', text)
    if not m:
        raise SystemExit('no [DisableSystems] section')
    body = m.group(1)
    for k in keys:
        body, n = re.subn(rf'(?m)^(\s*{k}\s*=\s*)\d+', r'\g<1>1', body)
        if n != 1:
            raise SystemExit(f'key {k}: {n} matches')
    CFG.write_bytes((text[:m.start(1)] + body + text[m.end(1):]).encode('latin1'))


def show():
    text = CFG.read_bytes().decode('latin1')
    m = re.search(r'(?ms)^\[DisableSystems\]\s*$(.*?)(?=^\[|\Z)', text)
    on = re.findall(r'(?m)^\s*(\w+)\s*=\s*1', m.group(1)) if m else []
    same = ORIG.exists() and sha(ORIG) == sha(CFG)
    print('disabled:', ' '.join(on) or '-')
    print('config.ini equals backup:', same, sha(CFG)[:16])


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    if a[0] == 'backup':
        BK.mkdir(parents=True, exist_ok=True)
        if ORIG.exists() and sha(ORIG) != sha(CFG):
            raise SystemExit('backup exists and differs from config.ini: restore first or inspect by hand')
        shutil.copy2(CFG, ORIG)
        print('backup', sha(ORIG))
    elif a[0] == 'set':
        if not ORIG.exists() or sha(ORIG) != sha(CFG):
            raise SystemExit('config.ini does not equal the backup: run restore (or backup) first')
        keys = PRESETS[a[1]] if len(a) == 2 and a[1] in PRESETS else a[1:]
        set_keys(keys)
        show()
    elif a[0] == 'restore':
        shutil.copy2(ORIG, CFG)
        ok = sha(ORIG) == sha(CFG)
        print('restored', sha(CFG)[:16], 'OK' if ok else 'MISMATCH')
        sys.exit(0 if ok else 1)
    elif a[0] == 'show':
        show()
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
