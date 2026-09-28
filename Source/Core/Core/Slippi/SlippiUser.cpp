#include "Common/Common.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "SlippiUser.h"

#include "SlippiRustExtensions.h"

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

// A copy rather than the launcher's file read in place: the account file is written to on login,
// logout and token refreshes, and two builds sharing one could log each other out. An existing copy
// is left alone so a token refreshed here is never replaced by an older one
void SlippiUser::AdoptLauncherAccount()
{
#ifdef _WIN32
	std::string dest = File::GetUserPath(F_USERJSON_IDX);
	if (File::Exists(dest))
		return;

	const char *appdata = getenv("APPDATA");
	if (!appdata)
		return;

	// The current launcher first, then older installs
	const char *paths[] = {
	    "\\Slippi Launcher\\netplay\\User\\Slippi\\user.json",
	    "\\Slippi Launcher\\playback\\User\\Slippi\\user.json",
	    "\\Slippi Desktop App\\dolphin\\User\\Slippi\\user.json",
	};

	for (const char *path : paths)
	{
		std::string src = std::string(appdata) + path;
		if (!File::Exists(src))
			continue;

		File::CreateFullPath(File::GetUserPath(D_SLIPPI_IDX));
		if (File::Copy(src, dest))
			INFO_LOG(SLIPPI_ONLINE, "Copied the Slippi account from %s", src.c_str());
		else
			WARN_LOG(SLIPPI_ONLINE, "Could not copy the Slippi account from %s", src.c_str());
		return;
	}

	WARN_LOG(SLIPPI_ONLINE, "No Slippi account here or in the launcher, matches can't start until one logs in");
#endif
}

bool SlippiUser::AttemptLogin()
{
	return slprs_user_attempt_login(slprs_exi_device_ptr);
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
	return slprs_user_get_is_logged_in(slprs_exi_device_ptr);
}
