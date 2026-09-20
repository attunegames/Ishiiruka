# First-run setup. Asks who you are, then never asks again.
#
# The three testers were previously three different downloads that differed only
# by two lines in a config file. This makes it one download.
#
# ⚠️ THIS SCRIPT TOUCHES NOTHING OUTSIDE THIS FOLDER. It writes one config file
# next to itself and nothing else. It used to go looking through %APPDATA% for
# the player's Slippi account and copy it in, which worked - and which is also
# indistinguishable from malware at a glance. A .bat shelling out to PowerShell
# with -ExecutionPolicy Bypass to read a credential file out of AppData is a
# fair thing for anyone to refuse to run, and testers were right to hesitate.
#
# The build does that itself now, in SlippiUser::AdoptLauncherAccount. The
# binary is the thing a tester has already decided to trust by running it, its
# source is public, and a Slippi build reading Slippi's own user.json is what
# anybody would expect it to do.

$ErrorActionPreference = "Stop"
$cfgDir  = Join-Path $PSScriptRoot "User\Config"
$cfgPath = Join-Path $cfgDir "peppy.json"

New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "Replays") | Out-Null

Write-Host ""
Write-Host "  PEPPY DOLPHIN - first time setup" -ForegroundColor Cyan
Write-Host "  --------------------------------"
Write-Host ""
Write-Host "  This writes one file, User\Config\peppy.json, in this folder."
Write-Host "  It does not read or change anything else on your computer."
Write-Host ""

# Melee shows 15 characters for a name and 8 for a connect code (TAG#123).
do {
    $name = (Read-Host "  What name do you want to play as").Trim()
} while ($name.Length -eq 0)
if ($name.Length -gt 15) { $name = $name.Substring(0, 15) }

$room = (Read-Host "  Room code [1234]").Trim()
if ($room.Length -eq 0) { $room = "1234" }

# A fallback connect code derived from the name: letters only, up to 4, plus 3
# digits so two people picking the same name still differ.
#
# ⚠️ IGNORED when you have a real Slippi account, which is the normal case - see
# SlippiUser::GetUserInfo, where a real login wins. It exists so a machine with
# no account can still reach the menus and the room screen. It CANNOT start a
# match: rooms are introduced by Slippi's own servers, and a code no account
# owns is accepted by the matchmaking server and then never assigned.
$tag = ($name.ToUpper() -replace '[^A-Z]', '')
if ($tag.Length -eq 0) { $tag = "PLYR" }
if ($tag.Length -gt 4) { $tag = $tag.Substring(0, 4) }
$code = "{0}#{1:D3}" -f $tag, (Get-Random -Minimum 1 -Maximum 999)

$cfg = [ordered]@{
    displayName  = $name
    connectCode  = $code
    supabaseUrl  = "https://aklpyoxkwnzcbtqjjuxk.supabase.co"
    supabaseKey  = "sb_publishable_PsmkI599Y7rt-Ty3jkVnbg_W_n6cq5O"
}
$cfg | ConvertTo-Json | Set-Content -Path $cfgPath -Encoding utf8

# Keep replays in this folder rather than the player's real Slippi one.
$dolphinIni = Join-Path $cfgDir "Dolphin.ini"
if (-not (Test-Path $dolphinIni)) {
    "[Core]`r`nSlippiReplayDir = .\Replays" | Set-Content -Path $dolphinIni -Encoding utf8
}

Write-Host ""
Write-Host ("  Saved. Playing as {0}, room {1}." -f $name, $room) -ForegroundColor Cyan
Write-Host "  If you are logged into the Slippi Launcher, the game picks that up"
Write-Host "  by itself and you play as your real connect code."
Write-Host ""
