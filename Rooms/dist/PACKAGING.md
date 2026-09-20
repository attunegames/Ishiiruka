# What ships to a tester, and what must not

These three files were living **only** in a rig folder on one desktop. They are
what a beta tester actually interacts with before Melee ever opens, and losing
them would have meant rewriting the first-run experience from memory.

## Building the zip

Take a rig folder and exclude, without exception:

| Excluded | Why |
|---|---|
| `*.iso` | Not ours to distribute. |
| `User/Slippi/user.json` | ⚠️ The developer's own Slippi account. |
| `User/Config/peppy.json` | ⚠️ Holds `refreshToken`, the identity, and `lanForTesting: true`. `setup.ps1` writes a clean one on first run. |
| `User/Logs/`, `Replays/` | Noise, and the logs name real players. |
| `*.peppy-bak`, `User/ishiiruka` | Local state. |

Then check the result before uploading:

    grep -ril "refreshToken\|playKey\|<your connect code>" <pkg>

⚠️ Escape the dots when grepping for a LAN address. `10.0.0.` as a regex matches
plenty of harmless bytes in a 21MB binary and will tell you the package is dirty
when it is not.

## The one that bites

`setup.ps1` invents a connect code from the player's name - `MIKE#472` - as a
development fallback for a machine with no Slippi account. Since rooms started
being introduced by Slippi's own servers, that code cannot start a match: the
matchmaking server accepts the ticket and never assigns it.

⚠️ It does not look broken. The room screen shows the invented name and code,
the player looks signed in, and they wait in the queue forever.

So the build finds the launcher's account and copies it in itself, in
`SlippiUser::AdoptLauncherAccount`. Precedence is in `SlippiUser::GetUserInfo`:
a real login wins, and `peppy.json` answers only when there is none.

## Nothing but the program ships

No installer, no `START.bat`, no `setup.ps1`. A tester unzips, drops in an ISO
and runs the exe.

That took two things off the scripts' hands:

- **The config.** `peppy.json` was written by a setup script. The Supabase URL
  and publishable key are compiled in as defaults now (`DEFAULT_SUPABASE_URL` /
  `DEFAULT_SUPABASE_KEY` in `SlippiRooms.cpp`), so a player never has one. The
  file still WINS when it exists, which is how a rig points at another project
  or turns `lanForTesting` on.
- **The account.** `SlippiUser::AdoptLauncherAccount` copies the launcher's
  `user.json` in on first run. The display name and connect code come from that,
  so there is nothing to type either.

The shipped `User/Config/Dolphin.ini` carries `ISOPath0 = .` so an ISO dropped
in the folder just appears in the game list, and `SlippiReplayDir = .\Replays`.
⚠️ Blank `LastFilename` before packaging - it otherwise ships the developer's
own ISO path.

⚠️ **Do not add a script back.** The first version was a `.bat` shelling out to
PowerShell with `-ExecutionPolicy Bypass` to copy a credential file out of
`%APPDATA%`. It worked, and it is also indistinguishable from malware at a
glance - a fair thing for a tester to refuse to run. Whatever the next
convenience is, it belongs in the binary, whose source is public.

## Why a copy and not the launcher's file in place

Pointing `user_config_folder` at the launcher's directory is a few lines -
`EXI_DeviceSlippi.cpp` hands that path to the Rust side - and it is the wrong
move. The Rust side WRITES there: login, logout, and the refresh token with
them. Two Dolphins sharing one credential file can rotate it out from under each
other, and what the tester sees is their real Slippi logging itself out. That
folder also holds `direct-codes.json` and an ISO cache, so reading in place
means this build writing into somebody's real Slippi install - the opposite of
what portable is for.

## GPL

The zip is binaries. The release notes must name the exact commits they were
built from, in both repos, and both must be public.
