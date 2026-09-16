#pragma once

// Peppy's own backend: who you are, and how to ask it for a room.
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
#include "Common/CommonTypes.h"

namespace Peppy
{
// Read from User/Config/peppy.json at startup. The refresh token is written
// back to it, which is what makes an install the same player twice.
struct Config
{
	std::string url;           // https://<ref>.supabase.co
	std::string key;           // publishable key - safe to ship, see above
	std::string name;          // display name: Alpha, Bravo, Charlie
	std::string refresh_token; // ours, written back after a sign-in
	bool loaded = false;
};

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

// Call a Postgres function by name. `args_json` is the argument object, and the
// result is whatever the function returned, as JSON text. An empty string means
// the call did not get through - the caller decides whether that is fatal.
std::string Rpc(const std::string &fn, const std::string &args_json);

// The round trip, end to end: make a room and hand back its code.
// Returns an empty string if anything went wrong, having said why in the log.
std::string CreateRoom(const std::string &mode, bool listed);
} // namespace Peppy
