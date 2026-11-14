@echo off
rem build.cmd - builds the Gate 3 compositor prototype from a clean shell.
rem   bin\shell_server.exe    (x64)
rem   bin\shell_composite.dll (x86)
rem   bin\test_host.exe       (x86)
rem Needs VS 2022 (MSVC v143) and Windows SDK 10.0.26100.0 (fxc). No downloads.
setlocal
set "ROOT=%~dp0"
if "%VSDIR%"=="" set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if "%SDKVER%"=="" set "SDKVER=10.0.26100.0"
set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\%SDKVER%\x64\fxc.exe"
set "GEN=%ROOT%build\gen"
set "BIN=%ROOT%bin"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%GEN%" mkdir "%GEN%"
if not exist "%BIN%" mkdir "%BIN%"
if not exist "%ROOT%build\obj64" mkdir "%ROOT%build\obj64"
if not exist "%ROOT%build\obj86" mkdir "%ROOT%build\obj86"

echo [1/4] shaders (fxc, shader model 4.0)
call :fxc vs_4_0 VSFullscreen || goto :fail
call :fxc ps_4_0 PSComposite || goto :fail
call :fxc ps_4_0 PSServer || goto :fail
call :fxc vs_4_0 VSHostUi || goto :fail
call :fxc ps_4_0 PSHostUi || goto :fail

echo [2/4] shell_server.exe (x64)
call :build64 || goto :fail
echo [3/4] shell_composite.dll + test_host.exe (x86)
call :build86 || goto :fail
echo [4/4] default config
if not exist "%BIN%\shell_composite.ini" copy /y "%ROOT%shell_composite.ini" "%BIN%\shell_composite.ini" >nul
echo BUILD OK: %BIN%
exit /b 0

:fail
echo BUILD FAILED
exit /b 1

:fxc
"%FXC%" /nologo /O3 /T %1 /E %2 /Fh "%GEN%\%2.h" /Vn g_%2 "%ROOT%src\shaders.hlsl" >nul || exit /b 1
exit /b 0

:build64
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE /I "%GEN%" "%ROOT%src\shell_server.cpp" ^
   /Fo"%ROOT%build\obj64\\" /Fd"%ROOT%build\obj64\\" /Fe"%BIN%\shell_server.exe" /link /DEBUG /OPT:REF || exit /b 1
endlocal & exit /b 0

:build86
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
rem /MT: static CRT, so the DLL brings no runtime dependency into the game process
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE /I "%GEN%" /LD ^
   "%ROOT%src\shell_composite.cpp" "%ROOT%src\vtable_index.c" ^
   /Fo"%ROOT%build\obj86\\" /Fd"%ROOT%build\obj86\\" /Fe"%BIN%\shell_composite.dll" /link /DEBUG /OPT:REF /SAFESEH || exit /b 1
cl /nologo /O2 /MT /W3 /EHsc /Zi /DUNICODE /D_UNICODE /I "%GEN%" "%ROOT%src\test_host.cpp" ^
   /Fo"%ROOT%build\obj86\\" /Fd"%ROOT%build\obj86\\" /Fe"%BIN%\test_host.exe" /link /DEBUG /OPT:REF /SUBSYSTEM:WINDOWS user32.lib shell32.lib || exit /b 1
endlocal & exit /b 0
