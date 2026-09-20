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
the player looks signed in, and they wait in the queue forever. So `setup.ps1`
copies the real `user.json` out of the Slippi Launcher itself, and shouts in
yellow when it cannot find one. A line in the README was not enough for a
failure this quiet.

Precedence is in `SlippiUser::GetUserInfo`: a real login wins, and `peppy.json`
answers only when there is none.

## GPL

The zip is binaries. The release notes must name the exact commits they were
built from, in both repos, and both must be public.
