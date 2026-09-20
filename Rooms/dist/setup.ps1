# First-run setup. Asks who you are, then never asks again.
#
# The three testers were previously three different downloads that differed only
# by two lines in a config file. This makes it one download.
#
# It also finds your Slippi account and copies it in, because that step being
# manual was the single most likely way a beta tester ended up stuck. A room
# needs a REAL connect code: two players are introduced by Slippi's own servers
# as a direct match, and a code no account owns is accepted by the matchmaking
# server and then never assigned. You wait forever and nothing says why.
#
# The invented code below is a FALLBACK for development - a machine with no
# Slippi account can still reach the menus and the room screen. It cannot start
# a match, and this script now says so loudly rather than letting it look fine.

$ErrorActionPreference = "Stop"
$cfgDir  = Join-Path $PSScriptRoot "User\Config"
$cfgPath = Join-Path $cfgDir "peppy.json"
$slipDir = Join-Path $PSScriptRoot "User\Slippi"
$slipDst = Join-Path $slipDir "user.json"

New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
New-Item -ItemType Directory -Force -Path $slipDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $PSScriptRoot "Replays") | Out-Null

Write-Host ""
Write-Host "  PEPPY DOLPHIN - first time setup" -ForegroundColor Cyan
Write-Host "  --------------------------------"
Write-Host ""

# ---------------------------------------------------------------- the account
$account = $null
if (Test-Path $slipDst) {
    $account = $slipDst
    Write-Host "  Slippi account: already here." -ForegroundColor Green
} else {
    # Where the Slippi Launcher keeps it. The first is current; the others are
    # older layouts that some installs still have.
    $candidates = @(
        (Join-Path $env:APPDATA "Slippi Launcher\netplay\User\Slippi\user.json"),
        (Join-Path $env:APPDATA "Slippi Launcher\playback\User\Slippi\user.json"),
        (Join-Path $env:APPDATA "Slippi Desktop App\dolphin\User\Slippi\user.json")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            Copy-Item -Path $c -Destination $slipDst -Force
            $account = $slipDst
            Write-Host "  Slippi account: copied from your Slippi Launcher." -ForegroundColor Green
            break
        }
    }
}

$realCode = $null
if ($account) {
    try {
        $u = Get-Content $account -Raw | ConvertFrom-Json
        if ($u.connectCode) { $realCode = $u.connectCode }
    } catch { }
}

if ($realCode) {
    Write-Host ("  Playing as {0}" -f $realCode) -ForegroundColor Green
    Write-Host ""
} else {
    Write-Host "  ***********************************************************" -ForegroundColor Yellow
    Write-Host "  *  NO SLIPPI ACCOUNT FOUND                                *" -ForegroundColor Yellow
    Write-Host "  *                                                         *" -ForegroundColor Yellow
    Write-Host "  *  You can open the menus and sit in a room, but a match   *" -ForegroundColor Yellow
    Write-Host "  *  will NEVER start. It will not look broken - you will    *" -ForegroundColor Yellow
    Write-Host "  *  just wait in the queue forever.                         *" -ForegroundColor Yellow
    Write-Host "  *                                                         *" -ForegroundColor Yellow
    Write-Host "  *  Fix it: copy user.json from                            *" -ForegroundColor Yellow
    Write-Host "  *    %APPDATA%\Slippi Launcher\netplay\User\Slippi\       *" -ForegroundColor Yellow
    Write-Host "  *  into                                                   *" -ForegroundColor Yellow
    Write-Host "  *    User\Slippi\   (in this folder)                      *" -ForegroundColor Yellow
    Write-Host "  *                                                         *" -ForegroundColor Yellow
    Write-Host "  *  Log in with the Slippi Launcher once if you never have.*" -ForegroundColor Yellow
    Write-Host "  ***********************************************************" -ForegroundColor Yellow
    Write-Host ""
}

# ------------------------------------------------------------------- the name
# Melee shows 15 characters for a name and 8 for a connect code (TAG#123).
do {
    $name = (Read-Host "  What name do you want to play as").Trim()
} while ($name.Length -eq 0)
if ($name.Length -gt 15) { $name = $name.Substring(0, 15) }

$room = (Read-Host "  Room code [1234]").Trim()
if ($room.Length -eq 0) { $room = "1234" }

# Derive a fallback connect code from the name: letters only, up to 4, plus 3
# digits so two people picking the same name still differ. ⚠ Ignored entirely
# when a real account is present - see SlippiUser::GetUserInfo.
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
if ($realCode) {
    Write-Host ("  Ready. You are {0} in room {1}." -f $realCode, $room) -ForegroundColor Cyan
} else {
    Write-Host ("  Saved as {0}, room {1} - but see the warning above." -f $name, $room) -ForegroundColor Yellow
}
Write-Host ""
