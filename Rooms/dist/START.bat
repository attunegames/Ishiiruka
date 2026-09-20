@echo off
REM Peppy Dolphin - Melee netplay with its own rooms.
REM
REM Portable: settings and replays stay in this folder. Your Slippi install is
REM read, never written - see refresh-account.ps1.
REM
REM You DO need a real Slippi account: two players in a room are introduced by
REM Slippi's own servers as a direct match, so a real connect code is required.
REM Your account is found and copied in automatically.
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

REM Take a fresh copy of the Slippi account if the launcher's is newer. Quiet,
REM every launch, so logging in again or a rotated token is picked up without
REM anybody having to know that is a thing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0refresh-account.ps1"

for %%f in (*.iso) do (
  start "" "Slippi Dolphin.exe" -e "%%~ff"
  exit /b
)
start "" "Slippi Dolphin.exe"
