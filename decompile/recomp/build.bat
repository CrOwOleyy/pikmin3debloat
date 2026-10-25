@echo off
rem Builds pikmin3.exe (Release, clang-cl + Ninja). Extra arguments are passed to
rem "cmake --build" (e.g. build.bat --target latte_assembler).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio 2022 ^(vswhere.exe^) not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio 2022 with the "Desktop development with C++" workload is required.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem clang-cl: LLVM installed separately, or the Clang component of Visual Studio
if exist "%ProgramFiles%\LLVM\bin\clang-cl.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang-cl >nul 2>nul || (
    echo clang-cl not found: install LLVM ^(https://github.com/llvm/llvm-project/releases^)
    echo or the "C++ Clang tools for Windows" component of Visual Studio.
    exit /b 1
)
cd /d "%~dp0"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build build %* || exit /b 1
