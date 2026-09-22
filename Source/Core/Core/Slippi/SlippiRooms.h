#pragma once

// Rooms's own backend: who you are, and how to ask it for a room.
//
// This replaces the connection to mm.slippi.gg for rooms only. It does the same
// job Slippi's server does - introduce two clients to each other and then get
// out of the way - and no game traffic passes through it. Every call is a
// SECURITY DEFINER function, so the publishable key compiled in here can do
// nothing on its own: it cannot read a room it is not in, or rewrite one it is.
//
// IDENTITY IS A PLACEHOLDER. Right now a client signs in anonymously and keeps
// the refresh token, so an install is the same player every launch and nobody
// has to make an account. Eventually this is the Slippi user, and the swap is
// SignIn() alone: everything above it reads Uid() and Name(), and the schema
// only ever sees auth.uid().
//
// Nothing durable is keyed to that identity yet - a member row cascades away
// with its room, and crowns live on the member row - so switching providers
// today costs nothing. The first cross-room player record is what turns it into
// a migration, and that is the point to decide rather than now.

#include <string>
#include <vector>
#include "Common/CommonTypes.h"

namespace Rooms
{
// Read from User/Config/peppy.json at startup. The refresh token is written
// back to it, which is what makes an install the same player twice.
struct Config
{
	std::string url;           // supabaseUrl: https://<ref>.supabase.co
	std::string key;           // supabaseKey: publishable, safe to ship, see above
	std::string name;          // displayName: Alpha, Bravo, Charlie
	std::string connect_code;  // connectCode: ALPH#694
	std::string refresh_token; // refreshToken: ours, written back after a sign-in

	// ⚠⚠ TEST RIGS ONLY - lanForTesting - DELETE BEFORE THE FIRST BETA ⚠⚠
	//
	// Three clients behind ONE router cannot reach each other by their shared
	// public address: that is a hairpin and most routers drop it. Slippi has the
	// same problem for the match itself and solves it by sending ipAddressLan
	// and using it when both externals agree - this is the same thing for the
	// watch connection, which Slippi knows nothing about.
	//
	// Off by default, and the LAN address is only ever dialled AFTER the real
	// one has failed, so a build with this off behaves exactly as if the code
	// were not here. Everything it touches is marked with this same banner.
	bool lan_for_testing = false;

	bool loaded = false;
};

// Who the room publishes us as.
//
// ⚠️ This has to be the REAL Slippi connect code once there is a login, because
// it is what the other client asks Slippi to connect to. A code out of
// peppy.json that no Slippi account owns is a DIRECT search for somebody who
// does not exist, and it fails as a timeout rather than as an error.
//
// Called before the heartbeat starts. Empty arguments leave the config's own
// values alone, which is what a development rig with no account falls back to.
void SetIdentity(const std::string &name, const std::string &connect_code);

// Loads the config and gets a session, reusing the stored refresh token when
// there is one and signing in anonymously when there is not. Safe to call more
// than once; it only works the first time.
//
// Blocks on the network, so call it off the CPU thread.
bool SignIn();

bool SignedIn();

// Empty until SignIn() has succeeded.
const std::string &Uid();
const std::string &Name();
const std::string &ConnectCode();

// Call a Postgres function by name. `args_json` is the argument object, and the
// result is whatever the function returned, as JSON text. An empty string means
// the call did not get through - the caller decides whether that is fatal.
std::string Rpc(const std::string &fn, const std::string &args_json);

// The round trip, end to end: make a room and hand back its code.
// Returns an empty string if anything went wrong, having said why in the log.
std::string CreateRoom(const std::string &mode, bool listed);

// ------------------------------------------------------------ the room view --

struct Player
{
	std::string name;
	std::string code;   // connect code
	int crowns = 0;
	// Where this player's netplay socket is, as "host:port", when they are in a
	// match and have published it. Only the two playing ever have one, and it is
	// what a watcher dials. pd_tick has carried it on the active roster since
	// part 2 - nothing had ever read it.
	std::string addr;
};

// What the top of the room draws.
//
// ⚠️ NOT_PICKED rather than 0, because 0 is a real character (internal id 0 is
// Captain Falcon) and a real stage. pd_tick returns null until somebody
// reports, deliberately, so the room can show a question mark instead of
// guessing - and a default of 0 would silently draw the wrong fighter.
struct Draft
{
	static constexpr int NOT_PICKED = -1;

	int stage = NOT_PICKED;
	int host_char = NOT_PICKED;
	int host_color = 0;
	int guest_char = NOT_PICKED;
	int guest_color = 0;

	// The pairing exists AND the match is on. The difference between "these two
	// are about to play" and "these two are playing", which is what decides
	// whether the band shows two fighters or stays empty.
	bool playing = false;

	// Whose box is open: 0 the winner of the last game, 1 the challenger,
	// 2 nobody. Derived server-side from which character columns are still
	// empty, so it cannot disagree with the picks themselves.
	static constexpr int TURN_NOBODY = 2;
	int pick_turn = TURN_NOBODY;

	// Seconds left on that box when this tick was answered.
	int pick_ends_in = 0;

	// ...and whether it is ours.
	bool pick_is_mine = false;

	// The question mark. ⚠️ A random pick is MASKED by pd_tick until the
	// match is on, so this is what arrives for one - the real fighter has
	// already been rolled and stored, and nobody is shown it until the splash.
	static constexpr int RANDOM = 254;
};

struct State
{
	bool valid = false;        // false until a tick has come back

	std::string room;

	// The room's own identity, asked for ONCE when the heartbeat starts,
	// because neither ever changes - see pd_room_identity. A private room has
	// a passcode and is not listed; a public one is listed and has none.
	std::string passcode;
	bool listed = true;

	std::string state;         // waiting | ready | stun | heartbeat | error
	std::string match_id;
	bool is_host = false;

	// Whether this room drafts its stages, and whether WE are the one allowed
	// to say so. A room is random by default - see Rooms/07-stage-draft.sql for
	// why that is the honest default rather than a button on the draft screen.
	//
	// ⚠ The owner can change hands while people are standing in the room, so
	// this rides every tick rather than being asked once.
	bool stage_draft = false;
	bool is_owner = false;

	// Who we are matched against, and their connect code - which is the whole
	// handoff: each side asks Slippi for a DIRECT match against the other's
	// code and Slippi makes the introduction it already makes for every direct
	// match. pd_members.code has carried this since part 1.
	std::string opponent_code;
	bool ready = false;         // the pairing is on, go and connect

	// The same thing, lifted out of `active` for the caller that dials them:
	// where the two playing can be reached, host first, blanks dropped.
	//
	// This is the netplay socket the match itself is running on, as Slippi's own
	// matchmaking server measured it - so a watcher dials an address nothing had
	// to go and discover.
	//
	// Empty until they are in a match and have published it.
	std::vector<std::string> watch_targets;

	// Everyone in this room who is trying to watch, as "host:port". The two
	// players send a packet at each of these so their own routers will let the
	// watcher's packets back in.
	//
	// ⚠ The server has been computing and returning this list since the
	// spectate work began, and nothing ever read it. Watching worked on a LAN,
	// where there is no NAT to open, and failed over the internet every time.
	std::vector<std::string> punch;

	std::vector<Player> active; // the two playing, host first
	std::vector<Player> queue;  // waiting, in the order the room will pair them
	std::vector<Player> lobby;  // present, not waiting for a game
	int position = 0;           // our place in that queue, 1-based

	Draft draft;
};

// Start and stop the heartbeat. Enter() spawns a thread that calls pd_tick
// every couple of seconds; Leave() stops it and tells the room we have gone.
//
// Enter() returns immediately - every tick happens on the heartbeat thread, so
// a two-second network call is never a stalled frame.
//
// ⚠️ Leave() BLOCKS, for as long as the current nap plus one round trip. It has
// to: the row has to actually go, and saying so from a detached thread would
// race an Enter() that followed it and delete the new room's membership. A
// scene change is the only place it is called and a short stall there is
// invisible.
void Enter(const std::string &room);
void Leave();

// Are we in a room at all? True from the moment the decision is made, which is
// what makes it useful: it waits neither for a tick to come back the way
// State::valid does, nor for the room's code to be known.
bool InRoom();

// Where Slippi says we can be reached, for watchers to dial. Reported on the
// next tick, like everything else here. Empty clears it.
//
// `lan` is TEST ONLY and is published only when peppy.json says lanForTesting -
// see Config::lan_for_testing. It rides along in the same field, after a space.
void SetAddress(const std::string &external, const std::string &lan);

// Where a STUN server saw this client's WATCH socket, as "host:port".
//
// ⚠ A watcher is the one client that cannot be told its own address any
// other way. Both players are handed theirs by Slippi's matchmaking server
// when the match is arranged; a watcher is in no match and gets nothing. The
// room publishes this so the two players can punch a hole towards it, because
// their routers drop anything from an address they have never sent to.
void SetWatchAddress(const std::string &external);

// ⚠⚠ TEST RIGS ONLY - DELETE BEFORE THE FIRST BETA ⚠⚠
bool LanForTesting();

// On our way into a room whose code we do not have yet.
//
// ⚠ Making a room is two network calls - sign in, then pd_room_create - and
// Enter() cannot be told the code until they finish. Say so at the moment the
// decision is made, so the room screen is not left guessing for the few hundred
// milliseconds in between, and take it back if the room never gets made.
void BeginEnter();
void AbandonEnter();

// Pressed Start, or stepped out of the queue. Takes effect on the next tick
// rather than immediately, which is why it returns nothing to check.
void SetQueued(bool queued);

/* Whether we have asked to be in the queue.
 *
 * ⚠️ Dolphin's answer, not the room's, and deliberately. pd_tick's position
 * is "how many are ahead of me, plus one", which is 1 for somebody standing
 * in the lobby who has pressed nothing - so it can never mean "not queued",
 * whatever the field has always claimed. This survives the game module being
 * reloaded, which a local flag over there does not. */
bool Queued();

// The owner turning the stage draft on or off. The server decides whether the
// caller may - see pd_set_stage_draft - and the answer comes back on the tick.
void SetStageDraft(bool on);

// What we picked, reported on the next tick. Each client may only report its
// OWN character; either of the two may report the stage.
void ReportPick(int character, int color, int stage);

// The match is over. Ends the pairing, moves the loser to the back of the
// queue and pairs whoever is next - all of which pd_result already does.
//
// ⚠ Until this is called the pairing stays 'ready', and a room whose pairing
// is ready is a room that wants to start a match. Both players walking back in
// after a game would be told to connect to each other all over again.
//
// Both sides report, and pd_result takes the first and answers "already
// recorded" to the second, so the result does not depend on the loser still
// being there to file it.
//
// Runs on its own thread: the caller is the game-end handler and must not wait
// on a round trip.
void ReportResult(const std::string &match_id, bool i_won, int winner_stocks);

// The most recent reply. Copied out under the lock, so the caller can read it
// at its leisure without holding anything up.
State Latest();

// --------------------------------------------------------------- browsing --

struct Listing
{
	std::string code;   // four characters, e.g. 8NXU
	std::string mode;   // singles | doubles | ironmans | crew | tournament
	std::string owner;  // who opened it
	int players = 0;    // how many are actually still talking to it
	int capacity = 8;   // the room's own limit, not a number baked in here
};

// Fetch the public rooms. `mode` filters; empty means every kind, which is what
// Public asks for - it is reached before a kind has been chosen, so each room
// says which it is instead.
//
// Blocking, so it runs on its own thread. The result goes where Rooms() can
// find it rather than coming back, because the caller is an EXI command that
// has to return to the game this frame.
void FetchRooms(const std::string &mode);

// What the last fetch found. Empty until one has come back - which is not the
// same as "there are no rooms", and the browser has to say so differently.
std::vector<Listing> Rooms();
bool RoomsFetched();
} // namespace Rooms
