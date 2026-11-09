@echo off
rem Builds and runs d3d11_hello.cpp for x64 and x86 with clang-cl (LLVM) + VS 2022 + Windows SDK 10.0.26100.0.
rem Output: %~dp0build\d3d11_hello_x64.exe and d3d11_hello_x86.exe
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
set "CLANGCL=C:\Program Files\LLVM\bin\clang-cl.exe"
set "SRC=%~dp0d3d11_hello.cpp"
set "OUT=%~dp0build"
if not exist "%OUT%" mkdir "%OUT%"
rem vcvarsall looks for vswhere.exe on PATH; without this it prints a harmless "not recognized" line.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"

setlocal
call "%VCVARS%" x64 10.0.26100.0 >nul || exit /b 1
echo [x64] "%CLANGCL%" -m64 /nologo /EHsc /O2 /W4 "%SRC%" /Fe"%OUT%\d3d11_hello_x64.exe" /Fo"%OUT%\d3d11_hello_x64.obj"
"%CLANGCL%" -m64 /nologo /EHsc /O2 /W4 "%SRC%" /Fe"%OUT%\d3d11_hello_x64.exe" /Fo"%OUT%\d3d11_hello_x64.obj" || exit /b 1
"%OUT%\d3d11_hello_x64.exe"
endlocal

setlocal
call "%VCVARS%" x86 10.0.26100.0 >nul || exit /b 1
echo [x86] "%CLANGCL%" -m32 /nologo /EHsc /O2 /W4 "%SRC%" /Fe"%OUT%\d3d11_hello_x86.exe" /Fo"%OUT%\d3d11_hello_x86.obj"
"%CLANGCL%" -m32 /nologo /EHsc /O2 /W4 "%SRC%" /Fe"%OUT%\d3d11_hello_x86.exe" /Fo"%OUT%\d3d11_hello_x86.obj" || exit /b 1
"%OUT%\d3d11_hello_x86.exe"
endlocal
endlocal
