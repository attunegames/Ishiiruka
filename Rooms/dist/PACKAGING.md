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

## Why the copy is not a script, and not read in place

Two separate decisions, both reversed once:

**Not a script.** It was a `.bat` shelling out to PowerShell with
`-ExecutionPolicy Bypass` to copy a credential file out of `%APPDATA%`. That
works, and it is also indistinguishable from malware at a glance - a fair thing
for a tester to refuse to run, and the user was right to flag it. The binary is
what they have already decided to trust, its source is public, and a Slippi
build reading Slippi's own `user.json` is expected behaviour.

⚠️ Keep it that way. Any future "just add a little script that..." lands in
the same place. The scripts that ship must touch nothing outside the folder,
and must stay short enough that a suspicious tester can read them.

**A copy, not the launcher's file in place.** Pointing `user_config_folder` at
the launcher's directory is a few lines - `EXI_DeviceSlippi.cpp` hands that path
to the Rust side - and it is the wrong move. The Rust side WRITES there: login,
logout, and the refresh token with them. Two Dolphins sharing one credential
file can rotate it out from under each other, and what the tester sees is their
real Slippi logging itself out. That folder also holds `direct-codes.json` and
an ISO cache, so reading in place means this build writing into somebody's real
Slippi install - the opposite of what portable is for.

## GPL

The zip is binaries. The release notes must name the exact commits they were
built from, in both repos, and both must be public.
