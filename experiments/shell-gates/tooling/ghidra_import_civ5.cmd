@echo off
rem Headless import + auto-analysis of the DX11 EXE copy into the Ghidra project "civ5".
rem Usage: ghidra_import_civ5.cmd            (import + analyse; run once)
rem The EXE is a COPY in C:\Users\Art\tools\ghidra-projects\bin, never the installed one.
setlocal
set "GHIDRA=C:\Users\Art\tools\ghidra-12.1.4"
set "JAVA_HOME=C:\Users\Art\tools\temurin-jdk-21.0.13+9"
set "GHIDRA_HEADLESS_MAXMEM=8G"
set "PROJ=C:\Users\Art\tools\ghidra-projects"
set "LOGS=%PROJ%\logs"
if not exist "%LOGS%" mkdir "%LOGS%"
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJ%\civ5" civ5 ^
  -import "%PROJ%\bin\CivilizationV_DX11.exe" ^
  -overwrite ^
  -analysisTimeoutPerFile 7200 ^
  -log "%LOGS%\import_civ5.log" ^
  -scriptlog "%LOGS%\import_civ5_script.log"
endlocal
