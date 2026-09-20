PEPPY DOLPHIN - BETA
====================

Melee netplay with its own ROOMS. Make a room, everyone joins it, two people
play and the rest wait in a queue and rotate in. The winner stays on.

This is a beta. Read the KNOWN ISSUES at the bottom before you report anything -
several of them are already known and being worked on.


WHAT YOU NEED
-------------

1. A REAL SLIPPI ACCOUNT - the one you already play ranked or direct with.
   This build connects you through Slippi's own servers as an ordinary direct
   match, so it needs a real connect code. There is no separate signup.

   START.bat finds your account and copies it in by itself. If you have ever
   logged in with the Slippi Launcher, there is nothing for you to do.

   If it cannot find one, it says so in a yellow box you cannot miss. Do not
   ignore that. You can still open the menus and sit in a room, but a match
   will NEVER start - and it does not look broken, you simply wait in the
   queue forever. Fix it by logging in with the Slippi Launcher once and
   running START.bat again, or copy the file yourself from

       %APPDATA%\Slippi Launcher\netplay\User\Slippi\user.json

   into   <this folder>\User\Slippi\

2. Your own Melee ISO, v1.02 (USA). Put it in this folder. Not included, and
   nobody will send you one.


HOW TO RUN
----------

1. Unzip anywhere. It is portable - it does not touch your Slippi install.
2. Put your ISO in this folder.
3. Run START.bat. The first time it asks what name you want.
4. In Melee:  1-P Mode  ->  Online Play  ->  the row below Party.
5. Make a room, or join one with a 4-letter code. Press START to join the queue.

The row below Party is Rooms. Its label reads "Log-in" because Melee's menu
labels are baked-in artwork and there are only eight of them - the ninth falls
back to the last one. Wrong word, right button.


HOW A ROOM WORKS
----------------

- Everyone with the same 4-letter code is in the same room.
- Press START to join the QUEUE. People not in the queue sit in the LOBBY.
- The two longest-waiting queue members play. The winner stays on, the loser
  goes to the back.
- Beat everyone in the room and you get a CROWN - and go to the back yourself,
  so the people waiting get their turn.
- Y watches the current match. Hold B to leave the room, hold Z to leave the
  queue.
- The room's host can press X to switch stages between RANDOM and a DRAFT.
  Random is the default: the stage picks itself and you only choose characters.


KNOWN ISSUES
------------

This is the first build anyone outside three machines on one network has run.
Everything below is known - please do not spend time reporting it.

- SPECTATING HAS NEVER BEEN TESTED OVER THE INTERNET. It works on a local
  network. Over the internet it is completely unproven and may not connect
  at all.

- If a game disconnects mid-match, the room can look stuck for up to fifteen
  minutes before it sorts itself out. Making a new room is faster.

- If Slippi's matchmaking does not answer, the room says "Connecting - trying
  again" and gives up after three attempts. If you see "Slippi could not
  connect us", leave the queue and rejoin.

- The character you played carries over between games. The COLOUR is supposed
  to as well; that fix is new and unproven.

- The stage roulette playing itself in the draft is deliberate.


WHAT IT DOES NOT TOUCH
----------------------

- Portable: settings and replays stay in this folder.
- Your Slippi install and launcher are not modified. Your user.json is COPIED
  out of the launcher, never moved or changed.
- Your user.json is read to log you in as yourself, and goes nowhere except
  Slippi's own servers - exactly as the normal Slippi build does.


REPORTING
---------

The useful report is: what you expected, what happened, and the room code.
If a match failed to start or a draft got stuck, grab the log at

    <this folder>\User\Logs\dolphin.log

before you close Dolphin - it is overwritten on the next launch.
