@echo off
rem Quake 4 (Xbox 360) for PC - one-click setup. Builds the game from your own ISO; see setup.ps1.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
