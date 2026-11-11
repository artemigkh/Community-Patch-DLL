@echo off
rem Process Monitor 4.13 (64-bit). NOTE: Procmon loads a kernel driver and needs elevation (UAC prompt).
start "" "C:\Users\Art\tools\procmon-4.13\Procmon64.exe" -accepteula %*
