# Keep this folder's copy of the Slippi account current.
#
# Runs on every launch, quietly. It exists because the alternative - pointing
# the build at the launcher's user.json directly - shares one credential file
# between two Dolphins, and the Slippi side WRITES to it (see
# slprs_user_listen_for_login / slprs_user_logout). Two processes rotating the
# same refresh token can invalidate each other, and the failure a tester would
# see is "this beta logged me out of my real Slippi". Not worth it to save a
# file copy.
#
# ⚠️ That folder is not only user.json either - it also holds direct-codes.json
# and iso-cache. Reading it in place would mean this build writing its own
# direct codes and cache into somebody's real Slippi install, which is exactly
# what "portable" is supposed to rule out.
#
# So: copy, but never go stale. Only when the launcher's file is NEWER, so a
# token this build refreshed for itself is not overwritten with an older one.

$ErrorActionPreference = "SilentlyContinue"

$dst = Join-Path $PSScriptRoot "User\Slippi\user.json"

$candidates = @(
    (Join-Path $env:APPDATA "Slippi Launcher\netplay\User\Slippi\user.json"),
    (Join-Path $env:APPDATA "Slippi Launcher\playback\User\Slippi\user.json"),
    (Join-Path $env:APPDATA "Slippi Desktop App\dolphin\User\Slippi\user.json")
)

$src = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $src) { exit 0 }   # nothing to copy from; setup.ps1 has already said so

if (-not (Test-Path $dst)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item -Path $src -Destination $dst -Force
    exit 0
}

$srcTime = (Get-Item $src).LastWriteTimeUtc
$dstTime = (Get-Item $dst).LastWriteTimeUtc
if ($srcTime -gt $dstTime) {
    Copy-Item -Path $src -Destination $dst -Force
}
