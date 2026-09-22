# Build the tester zip from a rig folder.
#
# ⚠️ THIS IS A DEVELOPER SCRIPT AND IS NEVER SHIPPED. Nothing a tester runs is a
# script - see PACKAGING.md. This one exists so the exclusions below are code
# rather than something somebody has to remember, because the cost of forgetting
# one is publishing a Slippi account or a Supabase refresh token.
#
#   .\pack.ps1 -Rig "C:\...\rooms-lan-test\Alpha" -Out "C:\...\PeppyDolphin-beta.zip"

param(
    [Parameter(Mandatory = $true)][string] $Rig,
    [Parameter(Mandatory = $true)][string] $Out,
    # ⚠️ The release tag this zip is going out as, written into VERSION.txt
    # so a freshly unzipped folder knows what it is. The updater reads it to say
    # "you are on X, updating to Y", and rewrites it afterwards. Left out, a
    # folder cannot name itself and the updater just says so.
    [string] $Version = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Rig)) { throw "no such rig folder: $Rig" }

$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("peppy-pack-" + [guid]::NewGuid().ToString("N"))
$pkg   = Join-Path $stage "PeppyDolphin-beta"
New-Item -ItemType Directory -Force -Path $pkg | Out-Null

# What must never ship, and why.
#   *.iso                     not ours to distribute
#   User\Slippi\user.json     the developer's own Slippi account
#   User\Config\peppy.json    refreshToken, identity, and lanForTesting = true
#   User\Logs, Replays        noise, and the logs name real players
#   the scripts               a tester runs the program, nothing else
$exclude = @(
    '\.iso$',
    'User\\Slippi\\user\.json$',
    'User\\Slippi\\direct-codes\.json$',
    'User\\Slippi\\iso-cache',
    'User\\Config\\peppy\.json$',
    # ⚠️ The rig's CONTROLLER mapping. Alpha drives port 1 from the keyboard so
    # the menus can be automated, and shipping that gave every tester a keyboard
    # on port 1 instead of their own controller. A real Slippi install has no
    # GCPadNew.ini at all - SIDevice 12 reads the adapter directly - so the right
    # thing to ship is nothing and let Dolphin decide.
    'User\\Config\\GCPadNew\.ini$',
    'User\\Logs\\',
    '^Replays\\',
    '\.peppy-bak$',
    # Rig-side spares: .known-good copies of the codeset, .bak modules. Harmless
    # but confusing in a tester's folder, and one of them being loaded instead of
    # the real file is exactly the kind of thing nobody would think to check.
    '\.known-good$',
    '\.bak$',
    # ⚠️ And the asset backups, which are worse than clutter. The loader prefers
    # a WHOLE MnMaAll.usd over its .diff patch, so a stray whole file silently
    # beats the patch that carries the Rooms menu artwork. 2.4 MB of them were
    # riding along in the first zip.
    '-backup$',
    'User\\ishiiruka',
    '^setup\.ps1$',
    '^START\.bat$',
    '^refresh-account\.ps1$',
    '^pack\.ps1$'
)

Push-Location $Rig
try {
    Get-ChildItem -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring((Get-Location).Path.Length + 1)
        foreach ($p in $exclude) { if ($rel -match $p) { return } }
        $dst = Join-Path $pkg $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        Copy-Item $_.FullName $dst
    }
} finally { Pop-Location }

# Empty folders the game expects to find.
New-Item -ItemType Directory -Force -Path (Join-Path $pkg "Replays") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkg "User\Slippi") | Out-Null

# The shipped Dolphin.ini points the game list at its own folder, so an ISO
# dropped in simply appears in it, and LastFilename is blanked - it otherwise
# ships the full path to the developer's own ISO.
#
# ⚠️ The `\r?` on each pattern is load-bearing. In .NET multiline, `$` matches
# before `\n`, so on a CRLF file the carriage return is still sitting there and
# an anchored match quietly fails. It did: the first zip went out with no ISO
# path at all, and only unpacking the result caught it.
$ini = Join-Path $pkg "User\Config\Dolphin.ini"
if (Test-Path $ini) {
    $t = Get-Content $ini -Raw
    $t = $t -replace '(?m)^LastFilename = .*?\r?$', 'LastFilename = '
    if ($t -match '(?m)^ISOPaths = 0\r?$') {
        $t = $t -replace '(?m)^ISOPaths = 0\r?$', "ISOPaths = 1`r`nISOPath0 = ."
    }
    Set-Content -Path $ini -Value $t -Encoding utf8 -NoNewline
}

# The README that ships is the one in this folder, not whatever the rig had.
Copy-Item (Join-Path $PSScriptRoot "README.txt") (Join-Path $pkg "README.txt") -Force

# The updater a tester double-clicks, built by the Updater workflow and dropped
# here. Optional: if it is missing the zip is still perfectly usable, it just
# means people unzip the next beta by hand.
#
# ⚠️ It is NOT in the rig, deliberately - the rigs are updated with
# deploy.sh and pointing an updater at them would fight that.
$upd = Join-Path $PSScriptRoot "updater\Update Peppy.exe"
if (Test-Path $upd) {
    Copy-Item $upd (Join-Path $pkg "Update Peppy.exe") -Force
    Write-Host "  included the updater" -ForegroundColor DarkGray
} else {
    Write-Host "  NO UPDATER - build the Updater workflow and put it in dist\updater\" -ForegroundColor Yellow
}

# ⚠️ Which build this is, inside the zip, where a tester can read it back to you.
# The first beta replaced its own asset four times under one filename, so nobody
# - including us - could tell which binary somebody was running. The first
# question after any report is "which build?", and there was no way to answer it.
$hash = {
    param($rel)
    $f = Join-Path $pkg $rel
    if (Test-Path $f) { (Get-FileHash $f -Algorithm MD5).Hash.Substring(0, 8).ToLower() } else { "missing" }
}
$verLines = @(
    "Peppy Dolphin - build identity",
    "",
    ("  version  " + $(if ($Version) { $Version } else { "unreleased" })),
    ("  exe      " + (& $hash 'Slippi Dolphin.exe')),
    ("  codeset  " + (& $hash 'Sys\GameSettings\GALE01r2.ini')),
    ("  module   " + (& $hash 'Sys\GameFiles\GALE01\SlippiRoom.dat')),
    ("  packed   " + (Get-Date -Format 'yyyy-MM-dd HH:mm')),
    "",
    "If you are reporting something, paste these lines. They say exactly which",
    "binaries you are running, which the release page alone cannot."
)
Set-Content -Path (Join-Path $pkg "VERSION.txt") -Value $verLines -Encoding utf8

# ⚠️ Check rather than trust. A regex here is worth more than a promise.
#
# ⚠️ Two different checks, because one alone is either not enough or too much.
# "refreshToken" and "playKey" appear in the EXE as JSON field names in compiled
# code, so searching every file for the bare word fails the build on its own
# source - it did on the first run. Those are looked for only as an assigned
# VALUE, and only in files that are text. Keys that could never be a field name
# are looked for everywhere.
$textExt  = @('.json', '.ini', '.txt', '.bat', '.ps1', '.cfg', '.xml', '.md')
$valuePat = @('"refreshToken"\s*:\s*"[^"]+"', '"playKey"\s*:\s*"[^"]+"',
              '"uid"\s*:\s*"[^"]+"', '"connectCode"\s*:\s*"[^"]+"')
$anyPat   = @('sb_secret', 'service_role', 'eyJhbGciOi[A-Za-z0-9._-]{30,}')

$leaks = @()
Get-ChildItem -Recurse -File $pkg | ForEach-Object {
    if ($_.Length -gt 40MB) { return }
    $raw = [IO.File]::ReadAllText($_.FullName)
    $rel = $_.FullName.Substring($pkg.Length + 1)
    foreach ($pat in $anyPat) {
        if ($raw -match $pat) { $leaks += ("{0}: {1}" -f $rel, $pat) }
    }
    if ($textExt -contains $_.Extension.ToLower()) {
        foreach ($pat in $valuePat) {
            if ($raw -match $pat) { $leaks += ("{0}: {1}" -f $rel, $pat) }
        }
    }
}
if ($leaks.Count -gt 0) {
    $leaks | ForEach-Object { Write-Host "  LEAK $_" -ForegroundColor Red }
    throw "refusing to package: something personal is in there"
}

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path $pkg -DestinationPath $Out
Write-Host ("  packaged {0:N1} MB -> {1}" -f ((Get-Item $Out).Length / 1MB), $Out) -ForegroundColor Green
Remove-Item -Recurse -Force $stage
