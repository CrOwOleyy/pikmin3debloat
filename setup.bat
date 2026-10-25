@echo off
rem Builds the Pikmin 3 (Wii U) recompilation from your own dump. See README.md.
rem Usage: setup.bat [-GameDir "<folder with the [Game] and [Update] folders>"] [-ForceLytShaders]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0decompile\setup.ps1" %*
exit /b %ERRORLEVEL%
