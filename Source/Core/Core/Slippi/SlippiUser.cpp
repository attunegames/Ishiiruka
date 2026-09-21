#include "Common/Common.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "SlippiUser.h"

#include "SlippiRustExtensions.h"

#include <fstream>
#include <mutex>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

// ---------------------------------------------------------- Rooms identity ---
//
// A DEVELOPMENT identity, and only that now.
//
// This used to replace the login outright, on the reasoning that rooms ran
// their own matchmaking so a slippi.gg account bought us nothing. That
// reasoning is dead: two players in a room are introduced by Slippi's own
// servers as a DIRECT match, which needs a real account and a real connect
// code. An identity we invented cannot be searched for by anybody.
//
// So the real login wins wherever there is one, and this answers only when
// there is not - which keeps a rig with no Slippi account able to reach the
// menus and the room screen. It cannot start a match, and that is the honest
// outcome rather than a failure three layers down inside matchmaking.
//
// This is NOT the Supabase identity. That one is anonymous, arrives over the
// network and lives in the Rooms backend; this is only what Melee is told so it
// will draw the menu.
namespace
{
std::once_flag s_rooms_once;
bool s_rooms_present = false;
SlippiUser::UserInfo s_rooms_user;

void LoadRoomsIdentity()
{
	std::call_once(s_rooms_once, []() {
		std::string path = File::GetUserPath(D_CONFIG_IDX) + "peppy.json";
		std::ifstream file(path);
		if (!file.good())
			return;

		try
		{
			json j;
			file >> j;
			s_rooms_user.displayName = j.value("displayName", "");
			s_rooms_user.connectCode = j.value("connectCode", "");
			s_rooms_user.uid = j.value("uid", "");
			s_rooms_user.playKey = "";

			// prepareOnlineStatus() semver-compares this against the running build
			// and tells Melee an update is required if it is newer. There is no
			// update service here, so report a version that can never win that
			// comparison.
			s_rooms_user.latestVersion = "0.0.0";

			s_rooms_present =
			    !s_rooms_user.displayName.empty() && !s_rooms_user.connectCode.empty();
			if (s_rooms_present)
			{
				WARN_LOG(SLIPPI_ONLINE, "[Rooms] Identity: %s (%s)",
				         s_rooms_user.displayName.c_str(), s_rooms_user.connectCode.c_str());
			}
		}
		catch (...)
		{
			// A malformed file must not take online mode down with it - fall back
			// to the normal login path.
			ERROR_LOG(SLIPPI_ONLINE, "[Rooms] Could not read %s, ignoring it", path.c_str());
		}
	});
}
} // namespace

// Takes a RustChatMessages pointer and extracts messages from them, then
// frees the underlying memory safely.
std::vector<std::string> ConvertChatMessagesFromRust(RustChatMessages *rsMessages)
{
	std::vector<std::string> chatMessages;

	for (int i = 0; i < rsMessages->len; i++)
	{
		std::string message = std::string(rsMessages->data[i]);
		chatMessages.push_back(message);
	}

	slprs_user_free_messages(rsMessages);

	return chatMessages;
}

SlippiUser::SlippiUser(uintptr_t rs_exi_device_ptr)
{
	slprs_exi_device_ptr = rs_exi_device_ptr;
}

SlippiUser::~SlippiUser() {}

// Take a copy of the Slippi account the launcher already has, if this folder
// has none of its own.
//
// ⚠ IN HERE rather than in a script beside the exe. It was a .bat shelling
// out to PowerShell with -ExecutionPolicy Bypass to copy a credential file out
// of AppData, which is indistinguishable from malware at a glance and a fair
// thing for a tester to refuse to run. This binary is the thing they already
// chose to trust, its source is public, and a Slippi build reading Slippi's own
// user.json is what anyone would expect it to do.
//
// ⚠ A COPY, not the launcher's file read in place. The Rust side WRITES to
// it - login, logout, and the refresh token with them - so two Dolphins sharing
// one credential file can rotate it out from under each other, and the failure a
// tester would see is their real Slippi logging itself out. That folder also
// holds direct-codes.json and an ISO cache, which this build has no business
// writing into.
//
// Only when we have none. An existing copy is left alone, so a token refreshed
// here is never replaced by an older one from the launcher.
void SlippiUser::AdoptLauncherAccount()
{
#ifdef _WIN32
	// ⚠️ Dolphin's own path indices, not a path built here. F_USERJSON_IDX IS
	// user.json, and D_SLIPPI_IDX already carries its trailing separator - the
	// first version of this pasted DIR_SEP in between and did not compile,
	// because that macro lives in CommonPaths.h and this file does not include
	// it. Asking for the index cannot drift from wherever Slippi really looks.
	std::string dir = File::GetUserPath(D_SLIPPI_IDX);
	std::string dest = File::GetUserPath(F_USERJSON_IDX);
	if (File::Exists(dest))
		return;

	const char *appdata = getenv("APPDATA");
	if (!appdata)
		return;

	// The current layout first, then two older ones some installs still have.
	const char *rel[] = {
	    "\\Slippi Launcher\\netplay\\User\\Slippi\\user.json",
	    "\\Slippi Launcher\\playback\\User\\Slippi\\user.json",
	    "\\Slippi Desktop App\\dolphin\\User\\Slippi\\user.json",
	};

	for (const char *r : rel)
	{
		std::string src = std::string(appdata) + r;
		if (!File::Exists(src))
			continue;
		File::CreateFullPath(dir);
		if (File::Copy(src, dest))
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] copied the Slippi account from %s", src.c_str());
		else
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] could NOT copy the Slippi account from %s", src.c_str());
		return;
	}

	WARN_LOG(SLIPPI_ONLINE, "[Rooms] no Slippi account here and none in the launcher - "
	                        "a match cannot start until one is logged in");
#endif
}

bool SlippiUser::AttemptLogin()
{
	// ⚠ Belt and braces. The real call is in the CEXISlippi constructor,
	// before the Rust device exists - here it is a no-op whenever the account
	// is already in place, which after that call it always is.
	AdoptLauncherAccount();

	// The REAL login first, and that ordering is the whole change.
	//
	// This used to answer from peppy.json and never open user.json at all,
	// because rooms ran their own matchmaking and a slippi.gg account bought us
	// nothing. That is no longer true: two players in a room are introduced by
	// Slippi's own servers as a DIRECT match, which needs a real account and a
	// real connect code. An identity we invented cannot be searched for.
	//
	// peppy.json stays as the FALLBACK, so a rig with no Slippi account still
	// reaches the menus and the room screen for development. It just cannot
	// start a match, which is the honest behaviour rather than a silent failure
	// three layers down.
	if (slprs_user_attempt_login(slprs_exi_device_ptr))
		return true;

	LoadRoomsIdentity();
	return s_rooms_present;
}

void SlippiUser::OpenLogInPage()
{
	slprs_user_open_login_page(slprs_exi_device_ptr);
}

void SlippiUser::ListenForLogIn()
{
	slprs_user_listen_for_login(slprs_exi_device_ptr);
}

bool SlippiUser::UpdateApp()
{
	return slprs_user_update_app(slprs_exi_device_ptr);
}

void SlippiUser::LogOut()
{
	slprs_user_logout(slprs_exi_device_ptr);
}

void SlippiUser::OverwriteLatestVersion(std::string version)
{
	slprs_user_overwrite_latest_version(slprs_exi_device_ptr, version.c_str());
}

SlippiUser::UserInfo SlippiUser::GetUserInfo()
{
	// A real login wins. Only when there is none does peppy.json answer.
	if (!slprs_user_get_is_logged_in(slprs_exi_device_ptr))
	{
		LoadRoomsIdentity();
		if (s_rooms_present)
			return s_rooms_user;
	}

	SlippiUser::UserInfo userInfo;

	RustUserInfo *info = slprs_user_get_info(slprs_exi_device_ptr);
	userInfo.uid = std::string(info->uid);
	userInfo.playKey = std::string(info->play_key);
	userInfo.displayName = std::string(info->display_name);
	userInfo.connectCode = std::string(info->connect_code);
	userInfo.latestVersion = std::string(info->latest_version);
	slprs_user_free_info(info);

	return userInfo;
}

std::vector<std::string> SlippiUser::GetDefaultChatMessages()
{
	RustChatMessages *chatMessages = slprs_user_get_default_messages(slprs_exi_device_ptr);
	return ConvertChatMessagesFromRust(chatMessages);
}

std::vector<std::string> SlippiUser::GetUserChatMessages()
{
	RustChatMessages *chatMessages = slprs_user_get_messages(slprs_exi_device_ptr);
	return ConvertChatMessagesFromRust(chatMessages);
}

bool SlippiUser::IsLoggedIn()
{
	if (slprs_user_get_is_logged_in(slprs_exi_device_ptr))
		return true;

	LoadRoomsIdentity();
	return s_rooms_present;
}
