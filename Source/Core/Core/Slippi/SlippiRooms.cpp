#include "Core/Slippi/SlippiRooms.h"

#include <curl/curl.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace Rooms
{
namespace
{
Config s_config;
std::string s_uid;
std::string s_access_token;
bool s_signed_in = false;
std::mutex s_mutex;
std::string s_watch_address;

// Where the rooms live, built in, so a player needs no config file at all.
//
// ⚠ The PUBLISHABLE key, and it belongs in the binary - that is what
// publishable means. It is safe here because of how the database is built, not
// because it is hidden: every table has RLS on with NO policies, which is
// deny-all, so nothing reaches a table directly. Every read and write goes
// through a SECURITY DEFINER function that checks auth.uid() and refuses when
// there is nobody signed in.
//
// ⚠ A SECRET key must never end up here. If one is ever needed, it belongs
// behind an edge function, not in a binary that ships to players.
//
// peppy.json still wins when it exists, which is how a test rig points itself
// at a different project or turns lanForTesting on.
static const char *DEFAULT_SUPABASE_URL = "https://aklpyoxkwnzcbtqjjuxk.supabase.co";
static const char *DEFAULT_SUPABASE_KEY = "sb_publishable_PsmkI599Y7rt-Ty3jkVnbg_W_n6cq5O";

std::string ConfigPath()
{
	return File::GetUserPath(D_CONFIG_IDX) + "peppy.json";
}

size_t WriteToString(char *data, size_t size, size_t count, void *out)
{
	static_cast<std::string *>(out)->append(data, size * count);
	return size * count;
}

// One place for every request, because every one of them needs the same two
// headers and the same failure handling.
//
// `bearer` is the access token when we have one. Supabase wants BOTH apikey and
// Authorization; sending only the key authenticates as nobody, and auth.uid()
// comes back null - which looks exactly like "not signed in" from the SQL side.
std::string Post(const std::string &url, const std::string &body, const std::string &bearer)
{
	CURL *curl = curl_easy_init();
	if (!curl)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] curl would not start");
		return "";
	}

	std::string response;
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, ("apikey: " + s_config.key).c_str());
	if (!bearer.empty())
		headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode res = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] %s failed: %s", url.c_str(), curl_easy_strerror(res));
		return "";
	}
	if (status < 200 || status >= 300)
	{
		// The body is where Postgres puts its reason, and it is almost always
		// the thing you actually want to read.
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] %s returned %ld: %s", url.c_str(), status,
		          response.c_str());
		return "";
	}
	return response;
}

bool LoadConfig()
{
	if (s_config.loaded)
		return true;

	// The defaults first. A player has no peppy.json and never makes one - the
	// file used to be written by a setup script, and asking somebody to run a
	// script before they can play is both friction and, fairly, suspicious.
	s_config.url = DEFAULT_SUPABASE_URL;
	s_config.key = DEFAULT_SUPABASE_KEY;

	std::string text;
	if (!File::ReadFileToString(ConfigPath(), text))
	{
		INFO_LOG(SLIPPI_ONLINE, "[Rooms] no peppy.json - using the built-in rooms");
		s_config.loaded = true;
		return true;
	}

	try
	{
		json j = json::parse(text);
		// The names peppy.json already uses, so one file serves both builds.
		s_config.url = j.value("supabaseUrl", s_config.url);
		s_config.key = j.value("supabaseKey", s_config.key);
		s_config.name = j.value("displayName", "");
		s_config.connect_code = j.value("connectCode", "");
		s_config.refresh_token = j.value("refreshToken", "");
		// ⚠ TEST RIGS ONLY. See Rooms::LanForTesting().
		s_config.lan_for_testing = j.value("lanForTesting", false);
		if (s_config.lan_for_testing)
		{
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] ⚠ lanForTesting is ON - this build publishes a LAN address. "
			                        "It must be off for anything that reaches real players.");
		}
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] peppy.json is not valid JSON: %s", e.what());
		return false;
	}

	if (s_config.url.empty() || s_config.key.empty())
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] peppy.json blanks supabaseUrl or supabaseKey. "
		                         "Remove the file to use the built-in ones.");
		return false;
	}
	if (s_config.name.empty())
		s_config.name = "Player";

	s_config.loaded = true;
	return true;
}

// Write the refresh token back. Without this every launch is a NEW anonymous
// user: the rig fills up with orphans and Alpha is not the same player twice.
void SaveRefreshToken(const std::string &token)
{
	s_config.refresh_token = token;

	json j;
	std::string text;
	if (File::ReadFileToString(ConfigPath(), text))
	{
		try
		{
			j = json::parse(text);
		}
		catch (const std::exception &)
		{
			j = json::object();
		}
	}
	j["refreshToken"] = token;

	if (!File::WriteStringToFile(j.dump(2), ConfigPath()))
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] could not write peppy.json - this install will be a "
		                        "different player next launch");
}

// Take whatever /auth/v1/token or /auth/v1/signup gave back.
bool AdoptSession(const std::string &response)
{
	if (response.empty())
		return false;
	try
	{
		json j = json::parse(response);
		s_access_token = j.value("access_token", "");
		std::string refresh = j.value("refresh_token", "");
		// find(), not contains() - the vendored nlohmann here is 3.4.0 and
		// contains() arrived in 3.11.
		auto user = j.find("user");
		if (user != j.end())
		{
			auto id = user->find("id");
			if (id != user->end() && id->is_string())
				s_uid = id->get<std::string>();
		}

		if (s_access_token.empty() || s_uid.empty())
			return false;
		if (!refresh.empty() && refresh != s_config.refresh_token)
			SaveRefreshToken(refresh);
		return true;
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the sign-in reply: %s", e.what());
		return false;
	}
}
} // namespace

bool SignedIn()
{
	return s_signed_in;
}

const std::string &Uid()
{
	return s_uid;
}

// ---------------------------------------------------------- who we really are
//
// ⚠ These are deliberately NOT in s_config. SetIdentity() used to write
// straight into it, and LoadConfig() then overwrote them again with peppy.json's
// invented name the next time somebody signed in. Which of the two won came down
// to whether the heartbeat had already signed in before the room screen said
// hello, so the same build published a real connect code on one rig and a
// made-up one on the other - and the other client then searched Slippi for a
// code no account owns, which fails as a silent timeout rather than an error.
//
// Slippi's answer outranks peppy.json wherever there is one.
std::string s_identity_name;
std::string s_identity_code;

const std::string &Name()
{
	return s_identity_name.empty() ? s_config.name : s_identity_name;
}

const std::string &ConnectCode()
{
	return s_identity_code.empty() ? s_config.connect_code : s_identity_code;
}

bool SignIn()
{
	std::lock_guard<std::mutex> lk(s_mutex);
	if (s_signed_in)
		return true;
	if (!LoadConfig())
		return false;

	// The stored token first, so an install keeps its identity. A refresh token
	// is single-use - the reply carries the next one, which AdoptSession saves.
	if (!s_config.refresh_token.empty())
	{
		std::string body = json{{"refresh_token", s_config.refresh_token}}.dump();
		if (AdoptSession(Post(s_config.url + "/auth/v1/token?grant_type=refresh_token", body, "")))
		{
			s_signed_in = true;
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] signed in as %s (%s), returning player",
			         s_config.name.c_str(), s_uid.c_str());
			return true;
		}
		// Expired or revoked. Fall through and become somebody new rather than
		// refusing to start - but say so, because it means losing this
		// install's identity and that is worth noticing in a log.
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] the stored token did not work - signing in fresh, "
		                        "this install is now a different player");
	}

	if (AdoptSession(Post(s_config.url + "/auth/v1/signup", "{}", "")))
	{
		s_signed_in = true;
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] signed in as %s (%s), new player", s_config.name.c_str(),
		         s_uid.c_str());
		return true;
	}

	ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not sign in. Anonymous sign-ins may be turned off "
	                         "for this project (Auth -> Providers).");
	return false;
}

void SetIdentity(const std::string &name, const std::string &connect_code)
{
	if (!name.empty())
		s_identity_name = name;
	if (!connect_code.empty())
		s_identity_code = connect_code;
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] playing as %s (%s)", Name().c_str(), ConnectCode().c_str());
}

std::string Rpc(const std::string &fn, const std::string &args_json)
{
	if (!SignedIn() && !SignIn())
		return "";
	return Post(s_config.url + "/rest/v1/rpc/" + fn, args_json, s_access_token);
}

std::string CreateRoom(const std::string &mode, bool listed)
{
	// ⚠️ Signed in BEFORE the arguments are built, not by Rpc afterwards.
	// SignIn() is what loads the config, and Name() and ConnectCode() read it -
	// so building args first sent an EMPTY name on the very first call of a
	// session, and the room was stored owned by nobody. That is not a display
	// bug and no amount of looking at the browser would have found it.
	if (!SignedIn() && !SignIn())
		return "";

	json args{{"p_mode", mode}, {"p_listed", listed}, {"p_name", Name()}, {"p_code", ConnectCode()}};

	std::string reply = Rpc("pd_room_create", args.dump());
	if (reply.empty())
		return "";

	try
	{
		json j = json::parse(reply);
		if (!j.value("ok", false))
		{
			ERROR_LOG(SLIPPI_ONLINE, "[Rooms] pd_room_create said no: %s",
			          j.value("error", "no reason given").c_str());
			return "";
		}
		std::string room = j.value("room", "");
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] made room %s (%s, %s)", room.c_str(), mode.c_str(),
		         listed ? "public" : "private");
		return room;
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the create reply: %s", e.what());
		return "";
	}
}
} // namespace Rooms

// ------------------------------------------------------------- the heartbeat --
//
// One thread, one call, every couple of seconds. pd_tick does everything in a
// single round trip: says we are still here, publishes our address, reports
// what we picked, arranges a match if one is due, and hands back the whole
// room. Anything that needs the room asks Latest() instead of the network.
//
// ⚠️ The network never happens on the CPU thread. A blocking POST there is a
// stalled frame, and at two seconds apart that would be visible.

namespace
{
std::thread s_tick_thread;
std::atomic<bool> s_ticking{false};

std::mutex s_state_lock;
Rooms::State s_state;

// Set by the game thread, read by the heartbeat. Small enough to be atomics
// rather than another lock.
std::string s_room;          // guarded by s_state_lock
std::atomic<bool> s_queued{false};

// On our way into a room, before we know its code.
//
// ⚠ Making a room is two network calls - sign in, then pd_room_create - and
// only then does Enter() have a code to store. s_room is empty for all of it,
// so "are we in a room" answered NO for a few hundred milliseconds and the room
// screen drew the public list before switching to the room it had just made.
// Joining never showed this because Enter() is called straight away, with the
// code already in hand.
std::atomic<bool> s_entering{false};

// Where watchers should dial us, guarded by s_state_lock like s_room.
std::string s_address;

// The match we have already played, so we do not play it twice.
//
// ⚠ pd_result ends the pairing, but the room scene is rebuilt the moment the
// game ends and ticks before the result has finished its round trip. For those
// couple of seconds the room still says 'ready', and a ready room is one that
// asks Slippi to connect us - to the person we have just finished playing.
//
// Guarded by s_state_lock, like s_room.
std::string s_played_match;
std::atomic<int> s_pick_char{Rooms::Draft::NOT_PICKED};
std::atomic<int> s_pick_color{0};
std::atomic<int> s_pick_stage{Rooms::Draft::NOT_PICKED};

// pd_tick returns null for anything not picked yet. nlohmann turns a null into
// a default-constructed value, so asking for an int gives 0 - which is a real
// character and a real stage. Read it as "absent means NOT_PICKED" instead.
int PickedOr(const json &j, const char *key)
{
	auto it = j.find(key);
	if (it == j.end() || it->is_null())
		return Rooms::Draft::NOT_PICKED;
	return it->get<int>();
}

void ReadReply(const json &j, Rooms::State &s);

// A string that may be SQL NULL.
//
// ⚠ json::value() is not safe for a nullable column. It only returns the
// default when the key is ABSENT - a key that is present and null still goes
// through get<std::string>(), which throws. Postgres sends every column of a
// json_build_object whether it has a value or not, so every nullable one is a
// key that is present and null.
//
// pd_members.addr is null until somebody is in a match and publishes it, which
// took down every client in the room the moment a pairing formed.
std::string Str(const json &j, const char *key)
{
	auto it = j.find(key);
	if (it == j.end() || !it->is_string())
		return "";
	return it->get<std::string>();
}

std::vector<Rooms::Player> ReadRoster(const json &j, const char *key)
{
	std::vector<Rooms::Player> out;
	auto it = j.find(key);
	if (it == j.end() || !it->is_array())
		return out;
	for (const auto &m : *it)
	{
		Rooms::Player p;
		p.name = Str(m, "name");
		p.code = Str(m, "code");
		p.crowns = m.value("crowns", 0);
		p.addr = Str(m, "addr");
		out.push_back(p);
	}
	return out;
}

void ApplyReply(const std::string &reply)
{
	// ⚠ The try covers READING the reply, not just parsing it.
	//
	// It used to stop at json::parse, and everything that picks the reply apart
	// sat outside it. This runs on the heartbeat thread, and an exception that
	// leaves a thread function is std::terminate - so one nullable column turned
	// into every client in the room disappearing, with nothing in the log and
	// only a 0xc0000409 in the Windows event viewer to say why.
	//
	// A tick we cannot read is a tick to skip. There will be another in two
	// seconds.
	Rooms::State s;
	try
	{
		json j = json::parse(reply);
		ReadReply(j, s);
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the tick reply: %s", e.what());
		return;
	}
	catch (...)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the tick reply");
		return;
	}

	std::lock_guard<std::mutex> lock(s_state_lock);
	// See s_played_match. The pairing is on its way out; do not start it again
	// while it goes.
	if (!s_played_match.empty() && s.match_id == s_played_match)
		s.ready = false;

	// ⚠️ Carried across. Everything below is rebuilt from the reply and then
	// assigned wholesale, and the room's code and passcode are NOT in a tick -
	// they are asked for once, by AskIdentityOnce. Without this they would be
	// wiped half a second after they arrived.
	s.passcode = s_state.passcode;
	s.listed = s_state.listed;

	s_state = s;
}

// Everything that picks a tick reply apart. Separated so the whole of it sits
// inside one try - see ApplyReply.
void ReadReply(const json &j, Rooms::State &s)
{
	s.valid = true;
	s.room = Str(j, "room");
	s.state = Str(j, "state");
	// ⚠ Null whenever there is no pairing, which is most of the time.
	s.match_id = Str(j, "matchId");
	s.is_host = j.value("isHost", false);
	s.stage_draft = j.value("stageDraft", false);
	s.is_owner = j.value("isOwner", false);
	s.position = j.value("position", 0);
	s.active = ReadRoster(j, "active");
	s.ready = s.state == "ready";

	auto opp = j.find("opponent");
	if (opp != j.end() && opp->is_object())
		s.opponent_code = Str(*opp, "code");
	// Lifted out of the active roster, which has carried it all along.
	for (const auto &p : s.active)
	{
		if (!p.addr.empty())
			s.watch_targets.push_back(p.addr);
	}

	// Who to punch towards, so their watcher can reach us.
	auto pu = j.find("punch");
	if (pu != j.end() && pu->is_array())
	{
		for (const auto &e : *pu)
		{
			if (e.is_string())
			{
				std::string a = e.get<std::string>();
				if (!a.empty())
					s.punch.push_back(a);
			}
		}
	}

	s.queue = ReadRoster(j, "queue");
	s.lobby = ReadRoster(j, "lobby");

	auto d = j.find("draft");
	if (d != j.end() && d->is_object())
	{
		s.draft.stage = PickedOr(*d, "stage");
		s.draft.host_char = PickedOr(*d, "hostChar");
		s.draft.guest_char = PickedOr(*d, "guestChar");
		s.draft.host_color = d->value("hostColor", 0);
		s.draft.guest_color = d->value("guestColor", 0);
		s.draft.playing = d->value("playing", false);
	}

}

void TickOnce()
{
	std::string room;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		room = s_room;
	}
	if (room.empty())
		return;

	// Same ordering trap as CreateRoom: sign in first, because Name() and
	// ConnectCode() are only filled once the config has been loaded.
	if (!Rooms::SignedIn() && !Rooms::SignIn())
		return;

	std::string addr, watch_addr;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		addr = s_address;
		watch_addr = s_watch_address;
	}

	json args{{"p_room", room},
	          {"p_name", Rooms::Name()},
	          {"p_code", Rooms::ConnectCode()},
	          {"p_queued", s_queued.load()}};

	// Only when there is one. pd_tick reads a missing p_addr as "unchanged",
	// which is what we want between matches - the address stays published while
	// the pairing lasts rather than being cleared by every idle tick.
	if (!addr.empty())
		args["p_addr"] = addr;

	// Same rule for the watch address: send it while we are watching, and let a
	// missing one mean "unchanged" rather than "stop". pd_tick has taken this
	// parameter since the spectate work began and nothing ever sent it.
	if (!watch_addr.empty())
		args["p_watch_addr"] = watch_addr;

	// Only send a pick when there is one. Sending null every tick would be
	// harmless - pd_tick ignores nulls - but it makes the log unreadable when
	// something does go wrong.
	int c = s_pick_char.load();
	int st = s_pick_stage.load();
	if (c != Rooms::Draft::NOT_PICKED)
	{
		args["p_char"] = c;
		args["p_color"] = s_pick_color.load();
	}
	if (st != Rooms::Draft::NOT_PICKED)
		args["p_stage"] = st;

	std::string reply = Rooms::Rpc("pd_tick", args.dump());
	if (reply.empty())
		return; // Rpc already said why.
	ApplyReply(reply);
}

// The room's own code and passcode, which never change.
//
// ⚠️ Once, on this thread, not on every tick and not in Enter(). Enter() is
// called from a scene change and returns immediately by design - a network
// round trip there is a stalled frame - and pd_tick already runs twice a
// second for everyone in every room, so an unchanging string has no business
// in it.
void AskIdentityOnce()
{
	std::string room;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		room = s_room;
	}
	if (room.empty())
		return;

	json args{{"p_room", room}};
	std::string reply = Rooms::Rpc("pd_room_identity", args.dump());
	if (reply.empty())
		return;

	try
	{
		json j = json::parse(reply);
		std::lock_guard<std::mutex> lock(s_state_lock);
		s_state.passcode = Str(j, "passcode");
		s_state.listed = j.value("listed", true);
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] %s is %s", room.c_str(),
		         s_state.listed ? "public" : "private");
	}
	catch (...)
	{
		// ⚠️ A room made before this function existed answers PGRST202 rather
		// than JSON. The corner shows no passcode and everything else carries on.
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] could not read the room's identity");
	}
}

void TickLoop()
{
	Common::SetCurrentThreadName("Rooms heartbeat");

	// ⚠️ AFTER the first tick, not before it. pd_room_identity only answers
	// somebody with a member row in the room - and the member row is inserted by
	// pd_tick itself, on its first call. Asked before that, it matched no rows,
	// returned null, and the read threw: "could not read the room's identity" on
	// every room anybody made.
	bool asked = false;

	while (s_ticking.load())
	{
		TickOnce();

		if (!asked)
		{
			asked = true;
			AskIdentityOnce();
		}

		// Two seconds, in short naps, so leaving a room does not wait out a
		// long sleep before the thread notices.
		for (int i = 0; i < 20 && s_ticking.load(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}
} // namespace

namespace Rooms
{
void Enter(const std::string &room)
{
	// ⚠ Did anyone announce this entry before now? BeginEnter is the moment the
	// decision was made, and everything the player has done SINCE belongs to
	// this room rather than the last one.
	//
	// Making a room is two network calls, and the room screen is up and taking
	// button presses for the whole of them - so pressing Start lands in that
	// window. Clearing the queue here unconditionally threw that press away:
	// the screen said "In the Queue" from its own flag while the heartbeat
	// reported us idle for ever, and one queued player never pairs with anyone.
	//
	// ⚠ Read BEFORE Leave(), which clears it.
	const bool announced = s_entering.load();

	Leave();
	s_entering.store(true);

	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		s_room = room;
		s_state = State(); // the previous room's view is not this room's
	}

	// Only the previous room's, and only when this entry came out of nowhere.
	if (!announced)
	{
		s_queued.store(false);
		s_pick_char.store(Draft::NOT_PICKED);
		s_pick_stage.store(Draft::NOT_PICKED);
	}

	s_ticking.store(true);
	s_tick_thread = std::thread(TickLoop);
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] heartbeat started for %s", room.c_str());
}

void Leave()
{
	s_entering.store(false);

	if (!s_ticking.load())
		return;

	s_ticking.store(false);
	if (s_tick_thread.joinable())
		s_tick_thread.join();

	std::string room;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		room = s_room;
		s_room.clear();
		s_state = State();
	}

	// Said once, after the thread has stopped, so it cannot race a tick that
	// would put the row straight back.
	if (!room.empty())
	{
		json args{{"p_room", room}};
		Rpc("pd_room_leave", args.dump());
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] left %s", room.c_str());
	}
}

bool InRoom()
{
	// Intent counts. See s_entering: the code arrives two network calls after
	// the decision, and the screen cannot wait that long to know what it is.
	if (s_entering.load())
		return true;
	std::lock_guard<std::mutex> lock(s_state_lock);
	return !s_room.empty();
}

void BeginEnter()
{
	s_entering.store(true);
}

void AbandonEnter()
{
	s_entering.store(false);
}

bool LanForTesting()
{
	return s_config.lan_for_testing;
}

void SetWatchAddress(const std::string &external)
{
	std::lock_guard<std::mutex> lock(s_state_lock);
	if (s_watch_address == external)
		return;
	s_watch_address = external;
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] publishing our watch address %s", external.c_str());
}

void SetAddress(const std::string &external, const std::string &lan)
{
	std::string addr = external;

	// ⚠⚠ TEST RIGS ONLY - DELETE BEFORE THE FIRST BETA ⚠⚠
	//
	// Rides along in the same field after a space, so no column and no migration
	// exists to be forgotten later. A reader that does not know about it sees
	// the real address followed by something it can ignore, and with the flag
	// off nothing is appended at all.
	if (s_config.lan_for_testing && !lan.empty())
		addr += " " + lan;

	std::lock_guard<std::mutex> lock(s_state_lock);
	if (s_address == addr)
		return;
	s_address = addr;
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] watchers can reach us at %s", addr.empty() ? "(nowhere)" : addr.c_str());
}

void SetQueued(bool queued)
{
	s_queued.store(queued);
}

bool Queued()
{
	return s_queued.load();
}

void ReportPick(int character, int color, int stage)
{
	// ⚠️ NOT_PICKED means "nothing to say about this one", NOT "clear it". The
	// draft arrives a step at a time and each step knows only its own field, so
	// storing the blanks would have every step wipe what the one before it
	// published - the character would vanish the moment a stage was chosen.
	//
	// Clearing is deliberate and happens in two places only: ReportResult, when
	// the game these picks belong to is over, and Enter, for a different room.
	if (character != Draft::NOT_PICKED)
	{
		s_pick_char.store(character);
		s_pick_color.store(color);
	}
	if (stage != Draft::NOT_PICKED)
		s_pick_stage.store(stage);
}

// The owner turning the stage draft on or off.
//
// ⚠ Fire and forget, on its own thread, like every other room RPC - the room
// screen must not stall waiting for a round trip. Nothing is written into the
// local state here either: the next tick brings the setting back, so the screen
// only ever shows what the server actually agreed to. A player who is not the
// owner changes nothing and their screen simply does not move.
void SetStageDraft(bool on)
{
	std::string room;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		room = s_room;
	}
	if (room.empty())
		return;

	std::thread([room, on]() {
		json args{{"p_room", room}, {"p_on", on}};
		std::string reply = Rpc("pd_set_stage_draft", args.dump());
		if (reply.empty())
			return; // Rpc already said why.
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] stage draft: %s", reply.c_str());
	}).detach();
}

void ReportResult(const std::string &match_id, bool i_won, int winner_stocks)
{
	if (match_id.empty())
		return;

	std::string room;
	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		room = s_room;
		s_played_match = match_id;
		// Whatever the next tick says, this pairing is finished.
		s_state.ready = false;
	}
	if (room.empty())
		return;

	// The picks belong to the game that just finished. Leaving them set would
	// hand the NEXT pairing this one's characters before anybody had chosen.
	s_pick_char.store(Draft::NOT_PICKED);
	s_pick_stage.store(Draft::NOT_PICKED);

	std::thread([room, match_id, i_won, winner_stocks]() {
		json args{{"p_room", room},
		          {"p_match_id", match_id},
		          {"p_i_won", i_won},
		          {"p_stocks", winner_stocks}};
		std::string reply = Rpc("pd_result", args.dump());
		if (reply.empty())
			return; // Rpc already said why.
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] reported the result: %s", reply.c_str());
	}).detach();
}

State Latest()
{
	std::lock_guard<std::mutex> lock(s_state_lock);
	return s_state;
}
} // namespace Rooms

// ----------------------------------------------------------------- browsing --

namespace
{
std::mutex s_rooms_lock;
std::vector<Rooms::Listing> s_rooms;
bool s_rooms_fetched = false;
} // namespace

namespace Rooms
{
void FetchRooms(const std::string &mode)
{
	json args = json::object();
	// Public is reached before a kind has been chosen, so it asks for every
	// kind. pd_room_list takes null for that, not a wildcard string.
	if (!mode.empty())
		args["p_mode"] = mode;

	std::string reply = Rpc("pd_room_list", args.dump());
	if (reply.empty())
		return; // Rpc already said why.

	std::vector<Listing> found;
	try
	{
		json j = json::parse(reply);
		auto rooms = j.find("rooms");
		if (rooms != j.end() && rooms->is_array())
		{
			for (const auto &r : *rooms)
			{
				Listing l;
				l.code = Str(r, "code");
				l.mode = Str(r, "mode");
				// A room whose owner never registered a name has a null here,
				// and one null would have thrown away the whole listing.
				l.owner = Str(r, "owner");
				l.players = r.value("players", 0);
				l.capacity = r.value("capacity", 8);
				found.push_back(l);
			}
		}
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the room list: %s", e.what());
		return;
	}

	{
		std::lock_guard<std::mutex> lock(s_rooms_lock);
		s_rooms = found;
		// ⚠️ Only set once something actually came back. "Nothing found yet" and
		// "there are no rooms" look the same on screen and want different words,
		// and a browser that says "no rooms" before its first reply is lying.
		s_rooms_fetched = true;
	}
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] %d public room(s)", (int)found.size());
}

std::vector<Listing> Rooms()
{
	std::lock_guard<std::mutex> lock(s_rooms_lock);
	return s_rooms;
}

bool RoomsFetched()
{
	std::lock_guard<std::mutex> lock(s_rooms_lock);
	return s_rooms_fetched;
}
} // namespace Rooms
