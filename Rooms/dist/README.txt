PEPPY DOLPHIN - BETA
====================

Melee netplay with its own ROOMS. Make a room, everyone joins it with a code,
two people play and the rest wait in a queue and rotate in. The winner stays on.

This is a beta. Read KNOWN ISSUES at the bottom before reporting anything.


HOW TO RUN
----------

1. Unzip anywhere.
2. Put your Melee ISO (v1.02 USA) in this folder. Not included.
3. Run "Slippi Dolphin.exe" and double-click Melee in the list.

That is the whole setup. No installer, no batch file, no script - just the
program. It is portable: settings, saves and replays stay in this folder.


FINDING ROOMS IN MELEE
----------------------

    1-P Mode  ->  Online Play  ->  ROOMS

ROOMS is the LAST row of that menu, below Log-in / Log-out / Update. It has its
own label, so look for the word rather than counting rows - which rows appear
depends on whether you are signed in.

Then pick one of three:

    Create a Room         make one and get a code to share
    Join by Room Code     type somebody's 4-character code
    Browse Public Rooms   see what is open right now

Creating asks what kind of room (Singles is the one that works today) and then
whether it is Public or Private. Private rooms take a 4-digit passcode.


IN A ROOM
---------

- Everyone with the same room code is in the same room.
- START joins the QUEUE. People who have not joined sit in the LOBBY.
- The two longest-waiting queue members play. The winner stays on, the loser
  goes to the back.
- Beat everyone in the room and you get a CROWN - and go to the back yourself,
  so the people waiting get their turn.
- START again, while queued, drops you into training on your own.
- Y watches the current match.
- Hold B to leave the room. Hold Z to leave the queue but stay in the room.
- Whoever MADE the room can press X to switch stages between RANDOM and a
  DRAFT. Random is the default: the stage picks itself in the draft and you
  only choose characters.


YOUR SLIPPI ACCOUNT
-------------------

You need one - the account you already play ranked or direct with. Two players
in a room are introduced by Slippi's OWN servers, as an ordinary direct match,
so a real connect code is required. There is no separate signup and nothing to
fill in.

The game takes a copy of the account the Slippi Launcher already has, the first
time it starts, and puts it in User\Slippi\ inside this folder. Nothing else on
your computer is read, and your Slippi install is never written to.

If you have never logged in with the Slippi Launcher, do that once and start
this again.

WITHOUT AN ACCOUNT the menus and the room screen still work, but a match will
NEVER start - and it does not look broken. You simply wait in the queue forever.
If that is happening to you, this is why.


KNOWN ISSUES
------------

This is the first build anyone outside three machines on one network has run.
All of this is known - please do not spend time reporting it.

- SPECTATING HAS NEVER BEEN TESTED OVER THE INTERNET. It works on a local
  network. Over the internet it is unproven and may not connect at all.

- If a game disconnects mid-match, the room can look stuck for up to fifteen
  minutes before it sorts itself out. Making a new room is faster.

- If Slippi's matchmaking does not answer, the room says "Connecting - trying
  again" and gives up after three attempts. If you see "Slippi could not
  connect us", leave the queue and rejoin.

- The character you played carries between games. The COLOUR is supposed to as
  well; that fix is new and unproven.

- The stage roulette playing itself in the draft is deliberate.

- Only Singles rooms work. The other kinds are in the menu and are not finished.


WHAT IT DOES TO YOUR COMPUTER
-----------------------------

- Portable. Settings, saves and replays stay in this folder.
- Your Slippi install and launcher are not modified. Your user.json is COPIED
  out of the launcher once, never moved or changed.
- user.json is read to log you in as yourself, and goes nowhere except Slippi's
  own servers - exactly as the normal Slippi build does.
- Room membership goes to this project's own database: your display name,
  connect code and the result of each game. Nothing else.


REPORTING
---------

What you expected, what happened, and the room code. If a match failed to start
or a draft got stuck, grab the log at

    <this folder>\User\Logs\dolphin.log

before you close Dolphin - it is overwritten on the next launch.
