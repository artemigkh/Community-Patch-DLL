@echo off
rem Process Explorer 17.21 (64-bit). Runs unelevated; some columns need elevation.
start "" "C:\Users\Art\tools\procexp-17.21\procexp64.exe" -accepteula %*
