@echo off
rem Builds the 32-bit helpers with clang-cl against the VS 2022 x86 SDK libraries.
rem Output: bin\stand_in.exe, bin\focus_guard.dll, bin\bg_helper32.exe
setlocal
set HERE=%~dp0
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set CC="C:\Program Files\LLVM\bin\clang-cl.exe" --target=i686-pc-windows-msvc /nologo /O2 /EHsc /MT /W3
if not exist "%HERE%bin" mkdir "%HERE%bin"
pushd "%HERE%bin"
if "%1"=="" goto all
goto %1
:all
:stand_in
%CC% "%HERE%src\stand_in.cpp" /Fe:stand_in.exe /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup || exit /b 1
if not "%1"=="" goto end
:focus_guard
if exist "%HERE%src\focus_guard.cpp" %CC% /LD "%HERE%src\focus_guard.cpp" /Fe:focus_guard.dll /link user32.lib || exit /b 1
if not "%1"=="" goto end
:bg_helper32
if exist "%HERE%src\bg_helper32.cpp" %CC% "%HERE%src\bg_helper32.cpp" /Fe:bg_helper32.exe /link user32.lib gdi32.lib advapi32.lib || exit /b 1
:end
del /q *.obj *.exp *.lib 2>nul
popd
echo build ok
