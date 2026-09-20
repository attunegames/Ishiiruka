@echo off
REM Peppy Dolphin - Melee netplay with its own rooms.
REM
REM This batch file only launches the game. The one-time setup below writes a
REM single config file inside this folder and reads nothing else on your
REM computer - see setup.ps1, which is plain text and short.
REM
REM You DO need a real Slippi account: two players in a room are introduced by
REM Slippi's own servers as a direct match, so a real connect code is required.
REM The game finds the account the Slippi Launcher already has and copies it in
REM by itself.
REM
REM Put your Melee ISO (v1.02) in this folder and it boots straight in.
REM Otherwise Dolphin opens and you point it at your ISO once.
REM
REM Then: 1-P Mode -> Online Play -> the bottom entry, below Party.

cd /d "%~dp0"

if not exist "User\Config\peppy.json" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
)

if not exist "User\Config\peppy.json" (
  echo.
  echo   Setup did not complete - no name was saved.
  echo   Run setup.ps1 directly to see what went wrong.
  echo.
  pause
  exit /b 1
)

for %%f in (*.iso) do (
  start "" "Slippi Dolphin.exe" -e "%%~ff"
  exit /b
)
start "" "Slippi Dolphin.exe"
