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

	std::string text;
	if (!File::ReadFileToString(ConfigPath(), text))
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] no peppy.json at %s", ConfigPath().c_str());
		return false;
	}

	try
	{
		json j = json::parse(text);
		// The names peppy.json already uses, so one file serves both builds.
		s_config.url = j.value("supabaseUrl", "");
		s_config.key = j.value("supabaseKey", "");
		s_config.name = j.value("displayName", "");
		s_config.connect_code = j.value("connectCode", "");
		s_config.refresh_token = j.value("refreshToken", "");
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] peppy.json is not valid JSON: %s", e.what());
		return false;
	}

	if (s_config.url.empty() || s_config.key.empty())
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] peppy.json needs supabaseUrl and supabaseKey");
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

const std::string &Name()
{
	return s_config.name;
}

const std::string &ConnectCode()
{
	return s_config.connect_code;
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

std::string Rpc(const std::string &fn, const std::string &args_json)
{
	if (!SignedIn() && !SignIn())
		return "";
	return Post(s_config.url + "/rest/v1/rpc/" + fn, args_json, s_access_token);
}

std::string CreateRoom(const std::string &mode, bool listed)
{
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

std::vector<Rooms::Player> ReadRoster(const json &j, const char *key)
{
	std::vector<Rooms::Player> out;
	auto it = j.find(key);
	if (it == j.end() || !it->is_array())
		return out;
	for (const auto &m : *it)
	{
		Rooms::Player p;
		p.name = m.value("name", "");
		p.code = m.value("code", "");
		p.crowns = m.value("crowns", 0);
		out.push_back(p);
	}
	return out;
}

void ApplyReply(const std::string &reply)
{
	json j;
	try
	{
		j = json::parse(reply);
	}
	catch (const std::exception &e)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] could not read the tick reply: %s", e.what());
		return;
	}

	Rooms::State s;
	s.valid = true;
	s.room = j.value("room", "");
	s.state = j.value("state", "");
	s.match_id = j.value("matchId", "");
	s.is_host = j.value("isHost", false);
	s.position = j.value("position", 0);
	s.active = ReadRoster(j, "active");
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

	std::lock_guard<std::mutex> lock(s_state_lock);
	s_state = s;
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

	json args{{"p_room", room},
	          {"p_name", Rooms::Name()},
	          {"p_code", Rooms::ConnectCode()},
	          {"p_queued", s_queued.load()}};

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

void TickLoop()
{
	Common::SetCurrentThreadName("Rooms heartbeat");

	while (s_ticking.load())
	{
		TickOnce();

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
	Leave();

	{
		std::lock_guard<std::mutex> lock(s_state_lock);
		s_room = room;
		s_state = State(); // the previous room's view is not this room's
	}
	s_queued.store(false);
	s_pick_char.store(Draft::NOT_PICKED);
	s_pick_stage.store(Draft::NOT_PICKED);

	s_ticking.store(true);
	s_tick_thread = std::thread(TickLoop);
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] heartbeat started for %s", room.c_str());
}

void Leave()
{
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

void SetQueued(bool queued)
{
	s_queued.store(queued);
}

void ReportPick(int character, int color, int stage)
{
	s_pick_char.store(character);
	s_pick_color.store(color);
	s_pick_stage.store(stage);
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
				l.code = r.value("code", "");
				l.mode = r.value("mode", "");
				l.owner = r.value("owner", "");
				l.players = r.value("players", 0);
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
