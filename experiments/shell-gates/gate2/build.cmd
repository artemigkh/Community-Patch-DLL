@echo off
rem build.cmd - Gate 2 tools.
rem   bin\shell_tap.dll      (x86) helper DLL for the game (crashlogs\shell.helper loader)
rem   bin\tap_matrix32.exe   (x86) sharing-matrix producer
rem   bin\tap_server.exe     (x64) sharing-matrix consumer, texture dumper, shader loader
rem Needs VS 2022 (MSVC v143) and the Windows SDK. No downloads.
setlocal
set "ROOT=%~dp0"
if "%VSDIR%"=="" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "BIN=%ROOT%bin"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%ROOT%build\obj64" mkdir "%ROOT%build\obj64"
if not exist "%ROOT%build\obj86" mkdir "%ROOT%build\obj86"
set "WHAT=%1"
if "%WHAT%"=="" set "WHAT=all"

if /i "%WHAT%"=="all" goto :b64
if /i "%WHAT%"=="x64" goto :b64
goto :b86
:b64
echo [x64] tap_server.exe
call :build64 || goto :fail
if /i "%WHAT%"=="x64" goto :ok
:b86
echo [x86] tap_matrix32.exe, tap_host32.exe, shell_tap.dll
call :build86 || goto :fail
:ok
echo BUILD OK: %BIN%
exit /b 0
:fail
echo BUILD FAILED
exit /b 1

:build64
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE "%ROOT%src\tap_server.cpp" ^
   /Fo"%ROOT%build\obj64\\" /Fd"%ROOT%build\obj64\\" /Fe"%BIN%\tap_server.exe" /link /DEBUG /OPT:REF || exit /b 1
endlocal & exit /b 0

:build86
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE "%ROOT%src\tap_matrix32.cpp" ^
   /Fo"%ROOT%build\obj86\\" /Fd"%ROOT%build\obj86\\" /Fe"%BIN%\tap_matrix32.exe" /link /DEBUG /OPT:REF || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE "%ROOT%src\tap_host32.cpp" ^
   /Fo"%ROOT%build\obj86\\" /Fd"%ROOT%build\obj86\\" /Fe"%BIN%\tap_host32.exe" /link /DEBUG /OPT:REF || exit /b 1
rem /MT: static CRT, so the DLL brings no runtime dependency into the game process
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE /LD "%ROOT%src\shell_tap.cpp" "%ROOT%src\tap_vtable.c" ^
   /Fo"%ROOT%build\obj86\\" /Fd"%ROOT%build\obj86\\" /Fe"%BIN%\shell_tap.dll" /link /DEBUG /OPT:REF /SAFESEH || exit /b 1
endlocal & exit /b 0
