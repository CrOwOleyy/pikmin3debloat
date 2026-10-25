@echo off
rem Starts the recompiled Pikmin 3 (TV + GamePad screens side by side). Extra arguments are
rem passed to the game, e.g. play.bat --no-sound
if not exist "%~dp0decompile\recomp\build\pikmin3.exe" (
    echo pikmin3.exe is not built yet: run setup.bat first.
    exit /b 1
)
"%~dp0decompile\recomp\build\pikmin3.exe" --split %*
