@echo off
rem VMMap 3.4. The 32-bit vmmap.exe is the one to use on the 32-bit game. Pass args through, e.g.
rem   vmmap.cmd -p ^<pid^> C:\path\out.mmp     (snapshot to file, no GUI)
rem   vmmap.cmd                                  (GUI)
"C:\Users\Art\tools\vmmap-3.4\vmmap.exe" -accepteula %*
