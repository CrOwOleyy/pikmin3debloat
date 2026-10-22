@echo off
rem Compile pikmin3.exe (Release, clang-cl + Ninja). Arguments supplémentaires passés à
rem « cmake --build » (ex. : build.bat --target decaf_gpu).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd /d "%~dp0"
if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build build -j 12 %* || exit /b 1
