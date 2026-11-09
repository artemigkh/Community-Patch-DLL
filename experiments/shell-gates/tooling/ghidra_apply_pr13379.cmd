@echo off
rem Applies pr13379_symbols.csv (EXE symbols from PR #13379) as labels to the analysed program in project civ5.
rem Runs on the existing program (-process, -noanalysis). Ghidra GUI must NOT have the project open (project lock).
setlocal
set "GHIDRA=C:\Users\Art\tools\ghidra-12.1.4"
set "JAVA_HOME=C:\Users\Art\tools\temurin-jdk-21.0.13+9"
set "GHIDRA_HEADLESS_MAXMEM=8G"
set "PROJ=C:\Users\Art\tools\ghidra-projects"
set "HERE=%~dp0"
call "%GHIDRA%\support\analyzeHeadless.bat" "%PROJ%\civ5" civ5 ^
  -process CivilizationV_DX11.exe -noanalysis ^
  -scriptPath "%HERE%ghidra_scripts" ^
  -postScript ApplyPr13379Symbols.java "%HERE%pr13379_symbols.csv" dx11 ^
  -log "%PROJ%\logs\apply_pr13379.log" ^
  -scriptlog "%PROJ%\logs\apply_pr13379_script.log"
endlocal
