// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/Debugger/Debugger_SymbolMap.h"

#include "Core/Slippi/SlippiRooms.h"
#include "Core/Slippi/SlippiPlayback.h"
#include "Core/Slippi/SlippiPremadeText.h"
#include "Core/Slippi/SlippiReplayComm.h"
#include <SlippiLib/SlippiGame.h>

#include <semver/include/semver200.h>
#include <algorithm> // std::min, in the room state reply
#include <utility> // std::move

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/HW/Memmap.h"

#include "AudioCommon/AudioCommon.h"
#include "VideoCommon/OnScreenDisplay.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/NetPlayClient.h"

#include "Core/HW/EXI_DeviceSlippi.h"
#include "Core/HW/SystemTimers.h"
#include "Core/State.h"

#include "Core/GeckoCode.h"
// #include "Core/PatchEngine.h"
#include "Core/PowerPC/PowerPC.h"

// Not clean but idk a better way atm
#include "DolphinWX/Frame.h"
#include "DolphinWX/Main.h"

// The Rust library that houses a "shadow" EXI Device that we can call into.
#include "SlippiRustExtensions.h"

#define FRAME_INTERVAL 900
#define SLEEP_TIME_MS 8
#define WRITE_FILE_SLEEP_TIME_MS 85

// #define LOCAL_TESTING

static std::unordered_map<u8, std::string> slippi_names;
static std::unordered_map<u8, std::string> slippi_connect_codes;

// ------------------------------------------------- Rooms: ending a session ---
//
// A finished room game has to put the match down before Melee is asked about it
// again, and the old branch found out the hard way what happens if it does not:
//
//   Melee asks prepareOnlineMatchState whether it has a match every time it
//   reaches the character select, and until the matchmaking state is CLEARED
//   the answer is still yes - the same match, long since disconnected. So Melee
//   starts it, it ends immediately, and the character select's request to go to
//   the room is overruled about fifty milliseconds later by the versus splash.
//
// Deferred rather than done on the spot, because the game-end handler is not a
// good place to destroy the objects the frame after it is still using them.
// ⚠ s_rooms_cleanup_busy is what makes it safe to search again: the old
// matchmaking and netplay clients are destroyed on a DETACHED thread and they
// still hold their port while they go.
std::atomic<bool> s_rooms_cleanup_busy(false);
bool s_rooms_end_session = false;
u64 s_rooms_end_session_at = 0;

bool RoomsCleanupBusy()
{
	return s_rooms_cleanup_busy.load();
}

// Unthrottled and silent, for a watcher that is behind.
//
// ⚠ The lever is the frame LIMITER, not the emulated CPU clock. Overclocking
// gives Melee more headroom inside each frame but the frames still arrive sixty
// a second, so it costs host CPU and buys no speed.
//
// Muted along with it, because Melee at several times speed does not read as
// catching up, it reads as broken. The viewer's own setting is put back
// afterwards rather than assumed.
static void setCatchUpSpeed(bool fast)
{
	static bool applied = false;
	static bool prevMuted = false;

	if (fast == applied)
		return;

	if (fast)
	{
		prevMuted = SConfig::GetInstance().m_IsMuted;
		SConfig::GetInstance().m_IsMuted = true;
	}
	else
	{
		SConfig::GetInstance().m_IsMuted = prevMuted;
	}
	AudioCommon::UpdateSoundStream();
	Core::SetIsThrottlerTempDisabled(fast);

	applied = fast;
	WARN_LOG(SLIPPI_ONLINE, "[Watch] catch-up %s", fast ? "on (throttle off, muted)" : "off");
}

// A watcher's opponent pads, out of the timeline instead of off a connection.
//
// Same shape Slippi already expects: newest frame first, one entry per frame
// back through the rollback window.
//
// ⚠ latestFrame is the newest frame held for BOTH players with no hole behind
// it, never the newest thing heard. Melee is told it may run to there and no
// further, so it never reaches a frame it has only half of - which is the
// divergence the old build died of, a missing player read as a neutral
// controller and the match wrong from that frame on.
static std::unique_ptr<SlippiRemotePadOutput> WatchRemotePad(SlippiWatchClient *watch, s32 frame, u8 port)
{
	auto out = std::make_unique<SlippiRemotePadOutput>();
	out->isDisconnected = false;
	out->checksumFrame = 0;
	out->checksum = 0;

	s32 latest = watch->LatestFrame();
	if (latest > frame)
		latest = frame; // never run ahead of the frame Melee is asking about
	out->latestFrame = latest;

	for (s32 f = latest; f > latest - ROLLBACK_MAX_FRAMES && f >= Slippi::GAME_FIRST_FRAME; f--)
	{
		u8 buf[SLIPPI_PAD_FULL_SIZE] = {};
		watch->GetPad(f, port, buf);
		out->data.insert(out->data.end(), buf, buf + SLIPPI_PAD_FULL_SIZE);
	}
	return out;
}


extern std::unique_ptr<SlippiPlaybackStatus> g_playbackStatus;
extern std::unique_ptr<SlippiReplayComm> g_replayComm;

#ifdef LOCAL_TESTING
bool isLocalConnected = false;
int localChatMessageId = 0;
#endif

// Are we waiting for input on this frame?
//  Is set to true between frames
bool g_needInputForFrame = false;

template <typename T> bool isFutureReady(std::future<T> &t)
{
	return t.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::vector<u8> uint16ToVector(u16 num)
{
	u8 byte0 = num >> 8;
	u8 byte1 = num & 0xFF;

	return std::vector<u8>({byte0, byte1});
}

std::vector<u8> uint32ToVector(u32 num)
{
	u8 byte0 = num >> 24;
	u8 byte1 = (num & 0xFF0000) >> 16;
	u8 byte2 = (num & 0xFF00) >> 8;
	u8 byte3 = num & 0xFF;

	return std::vector<u8>({byte0, byte1, byte2, byte3});
}

std::vector<u8> int32ToVector(int32_t num)
{
	u8 byte0 = num >> 24;
	u8 byte1 = (num & 0xFF0000) >> 16;
	u8 byte2 = (num & 0xFF00) >> 8;
	u8 byte3 = num & 0xFF;

	return std::vector<u8>({byte0, byte1, byte2, byte3});
}

void appendWordToBuffer(std::vector<u8> *buf, u32 word)
{
	auto wordVector = uint32ToVector(word);
	buf->insert(buf->end(), wordVector.begin(), wordVector.end());
}

void appendHalfToBuffer(std::vector<u8> *buf, u16 word)
{
	auto halfVector = uint16ToVector(word);
	buf->insert(buf->end(), halfVector.begin(), halfVector.end());
}

std::string processDiff2(std::vector<u8> iState, std::vector<u8> cState)
{
	INFO_LOG(SLIPPI, "Processing diff");
	std::string diff = std::string();
	open_vcdiff::VCDiffEncoder encoder((char *)iState.data(), iState.size());
	encoder.Encode((char *)cState.data(), cState.size(), &diff);

	INFO_LOG(SLIPPI, "done processing");
	return diff;
}

std::string ConvertConnectCodeForGame(const std::string &input)
{
	char fullWidthShiftJisHashtag[] = {(char)0x81, (char)0x94, (char)0x00};
	std::string connectCode(input);
	connectCode = ReplaceAll(connectCode, "#", fullWidthShiftJisHashtag);
	connectCode.resize(CONNECT_CODE_LENGTH + 2); // fixed length + full width (two byte) hashtag +1, null terminator +1
	return connectCode;
}

// This function gets passed to the Rust EXI device to support emitting OSD messages
// across the Rust/C/C++ boundary.
void OSDMessageHandler(const char *message, u32 color, u32 duration_ms)
{
	// When called with a C str type, this constructor does a copy.
	//
	// We intentionally do this to ensure that there are no ownership issues with a C String coming
	// from the Rust side. This isn't a particularly hot code path so we don't need to care about
	// the extra allocation, but this could be revisited in the future.
	std::string msg(message);

	OSD::AddMessage(msg, duration_ms, color);
}

CEXISlippi::CEXISlippi()
{
	INFO_LOG(SLIPPI, "EXI SLIPPI Constructor called.");

	// @TODO: For mainline port, ISO file path can't be fetched this way. Look at the following:
	// https://github.com/dolphin-emu/dolphin/blob/7f450f1d7e7d37bd2300f3a2134cb443d07251f9/Source/Core/Core/Movie.cpp#L246-L249
	std::string isoPath = SConfig::GetInstance().m_strFilename;

	// @TODO: Eventually we should move `GetSlippiUserConfigFolder` out of the File module.
	std::string userConfigFolder = File::GetSlippiUserConfigFolder();

	// ⚠ BEFORE the Rust device, not after. The Rust side is handed
	// user_config_folder below and reads the Slippi account THEN, once. This
	// used to be called from SlippiUser::AttemptLogin, about seven seconds
	// later, so a first run on a clean machine found no account: Melee drew its
	// "Log-in" screen with no Rooms row at all, and then worked perfectly on
	// every later run, because by then the file was already in place. A bug that
	// only happens once per machine is one every new player hits and nobody
	// testing a second time can see.
	SlippiUser::AdoptLauncherAccount();

	SlippiRustEXIConfig slprs_exi_config;
	slprs_exi_config.iso_path = isoPath.c_str();
	slprs_exi_config.user_config_folder = userConfigFolder.c_str();
	slprs_exi_config.scm_slippi_semver_str = scm_slippi_semver_str.c_str();
	slprs_exi_config.osd_add_msg_fn = OSDMessageHandler;

	slprs_exi_device_ptr = slprs_exi_device_create(slprs_exi_config);

	m_slippiserver = SlippiSpectateServer::getInstance();
	user = std::make_unique<SlippiUser>(slprs_exi_device_ptr);
	g_playbackStatus = std::make_unique<SlippiPlaybackStatus>();
	matchmaking = std::make_unique<SlippiMatchmaking>(slprs_exi_device_ptr, user.get());
	gameFileLoader = std::make_unique<SlippiGameFileLoader>();
	g_replayComm = std::make_unique<SlippiReplayComm>();
	directCodes = std::make_unique<SlippiDirectCodes>(slprs_exi_device_ptr, SlippiDirectCodes::DIRECT);
	teamsCodes = std::make_unique<SlippiDirectCodes>(slprs_exi_device_ptr, SlippiDirectCodes::TEAMS);

	generator = std::default_random_engine(Common::Timer::GetTimeMs());

	shouldOutput = SConfig::GetInstance().m_coutEnabled && g_replayComm->getSettings().mode != "mirror";

	// Loggers will check 5 bytes, make sure we own that memory
	m_read_queue.reserve(5);

	// Initialize local selections to empty
	localSelections.Reset();

	// Forces savestate to re-init regions when a new ISO is loaded
	SlippiSavestate::shouldForceInit = true;

	// Update user file and then listen for User
#ifndef IS_PLAYBACK
	user->ListenForLogIn();
#endif

	// Use sane stage defaults (should get overwritten)
	allowedStages = {
	    0x2,  // FoD
	    0x3,  // Pokemon
	    0x8,  // Yoshi's Story
	    0x1C, // Dream Land
	    0x1F, // Battlefield
	    0x20, // Final Destination
	};

	//    auto spt = SlippiPremadeText();
	//    spt.GetPremadeTextData(SlippiPremadeText::SPT_CHAT_P1, "Rapito", "Test");
	//    spt.GetPremadeTextData(SlippiPremadeText::SPT_CHAT_P1, "ラピト", "Test");
}

CEXISlippi::~CEXISlippi()
{
	u8 empty[1];

	// Closes file gracefully to prevent file corruption when emulation
	// suddenly stops. This would happen often on netplay when the opponent
	// would close the emulation before the file successfully finished writing
	writeToFileAsync(&empty[0], 0, "close");
	writeThreadRunning = false;
	if (m_fileWriteThread.joinable())
	{
		m_fileWriteThread.join();
	}
	m_slippiserver->endGame(true);

	// Try to determine whether we were playing an in-progress ranked match, if so
	// indicate to server that this client has abandoned. Anyone trying to modify
	// this behavior to game their rating is subject to get banned.
	auto activeMatchId = matchmaking->GetMatchmakeResult().id;
	if (activeMatchId.find("mode.ranked") != std::string::npos)
	{
		ERROR_LOG(SLIPPI_ONLINE, "Exit during in-progress ranked game: %s", activeMatchId.c_str());

		slprs_exi_device_report_match_status(slprs_exi_device_ptr, activeMatchId.c_str(), "abandoned", false);
	}
	handleConnectionCleanup();

	localSelections.Reset();

	// Kill threads to prevent cleanup crash
	g_playbackStatus->resetPlayback();

	// Instruct the Rust EXI device to shut down/drop everything.
	slprs_exi_device_destroy(slprs_exi_device_ptr);

	// TODO: ENET shutdown should maybe be done at app shutdown instead.
	// Right now this might be problematic in the case where someone starts a netplay client
	// and then queues into online matchmaking, and then stops the game. That might deinit
	// the ENET libraries so that they can't be used anymore for the netplay lobby? Course
	// you'd have to be kinda dumb to do that sequence of stuff anyway so maybe it's nbd
	if (isEnetInitialized)
		enet_deinitialize();
}

void CEXISlippi::configureCommands(u8 *payload, u8 length)
{
	for (int i = 1; i < length; i += 3)
	{
		// Go through the receive commands payload and set up other commands
		u8 commandByte = payload[i];
		u32 commandPayloadSize = payload[i + 1] << 8 | payload[i + 2];
		payloadSizes[commandByte] = commandPayloadSize;
	}
}

void CEXISlippi::updateMetadataFields(u8 *payload, u32 length)
{
	if (length <= 0 || payload[0] != CMD_RECEIVE_POST_FRAME_UPDATE)
	{
		// Only need to update if this is a post frame update
		return;
	}

	// Keep track of last frame
	lastFrame = payload[1] << 24 | payload[2] << 16 | payload[3] << 8 | payload[4];

	// Keep track of character usage
	u8 playerIndex = payload[5];
	u8 internalCharacterId = payload[7];
	if (!characterUsage.count(playerIndex) || !characterUsage[playerIndex].count(internalCharacterId))
	{
		characterUsage[playerIndex][internalCharacterId] = 0;
	}
	characterUsage[playerIndex][internalCharacterId] += 1;
}

std::unordered_map<u8, std::string> CEXISlippi::getNetplayNames()
{
	std::unordered_map<u8, std::string> names;

	if (slippi_names.size())
	{
		names = slippi_names;
	}

	else if (netplay_client && netplay_client->IsConnected())
	{
		auto netplayPlayers = netplay_client->GetPlayers();
		for (auto it = netplayPlayers.begin(); it != netplayPlayers.end(); ++it)
		{
			auto player = *it;
			u8 portIndex = netplay_client->FindPlayerPad(player);
			if (portIndex < 0)
			{
				continue;
			}

			names[portIndex] = player->name;
		}
	}

	return names;
}

std::vector<u8> CEXISlippi::generateMetadata()
{
	std::vector<u8> metadata({'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'});

	// TODO: Abstract out UBJSON functions to make this cleaner

	// Add game start time
	u8 dateTimeStrLength = sizeof "2011-10-08T07:07:09Z";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%FT%TZ", gmtime(&gameStartTime));
	dateTimeBuf.pop_back(); // Removes the \0 from the back of string
	metadata.insert(metadata.end(), {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (u8)dateTimeBuf.size()});
	metadata.insert(metadata.end(), dateTimeBuf.begin(), dateTimeBuf.end());

	// Add game duration
	std::vector<u8> lastFrameToWrite = int32ToVector(lastFrame);
	metadata.insert(metadata.end(), {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'});
	metadata.insert(metadata.end(), lastFrameToWrite.begin(), lastFrameToWrite.end());

	// Add players elements to metadata, one per player index
	metadata.insert(metadata.end(), {'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{'});

	auto playerNames = getNetplayNames();

	for (auto it = characterUsage.begin(); it != characterUsage.end(); ++it)
	{
		auto playerIndex = it->first;
		auto playerCharacterUsage = it->second;

		metadata.push_back('U');
		std::string playerIndexStr = std::to_string(playerIndex);
		metadata.push_back((u8)playerIndexStr.length());
		metadata.insert(metadata.end(), playerIndexStr.begin(), playerIndexStr.end());
		metadata.push_back('{');

		// Add names element for this player
		metadata.insert(metadata.end(), {'U', 5, 'n', 'a', 'm', 'e', 's', '{'});

		if (playerNames.count(playerIndex))
		{
			auto playerName = playerNames[playerIndex];
			// Add netplay element for this player name
			metadata.insert(metadata.end(), {'U', 7, 'n', 'e', 't', 'p', 'l', 'a', 'y', 'S', 'U'});
			metadata.push_back((u8)playerName.length());
			metadata.insert(metadata.end(), playerName.begin(), playerName.end());
		}

		if (slippi_connect_codes.count(playerIndex))
		{
			auto connectCode = slippi_connect_codes[playerIndex];
			// Add connection code element for this player name
			metadata.insert(metadata.end(), {'U', 4, 'c', 'o', 'd', 'e', 'S', 'U'});
			metadata.push_back((u8)connectCode.length());
			metadata.insert(metadata.end(), connectCode.begin(), connectCode.end());
		}

		metadata.push_back('}'); // close names

		// Add character element for this player
		metadata.insert(metadata.end(), {'U', 10, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 's', '{'});
		for (auto it2 = playerCharacterUsage.begin(); it2 != playerCharacterUsage.end(); ++it2)
		{
			metadata.push_back('U');
			std::string internalCharIdStr = std::to_string(it2->first);
			metadata.push_back((u8)internalCharIdStr.length());
			metadata.insert(metadata.end(), internalCharIdStr.begin(), internalCharIdStr.end());

			metadata.push_back('l');
			std::vector<u8> frameCount = uint32ToVector(it2->second);
			metadata.insert(metadata.end(), frameCount.begin(), frameCount.end());
		}
		metadata.push_back('}'); // close characters

		metadata.push_back('}'); // close player
	}
	metadata.push_back('}');

	// Indicate this was played on dolphin
	metadata.insert(metadata.end(),
	                {'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U', 7, 'd', 'o', 'l', 'p', 'h', 'i', 'n'});

	metadata.push_back('}');
	return metadata;
}

void CEXISlippi::writeToFileAsync(u8 *payload, u32 length, std::string fileOption)
{
#ifndef IS_PLAYBACK
	bool shouldSaveReplays = SConfig::GetInstance().m_slippiSaveReplays;
#else
	bool shouldSaveReplays = SConfig::GetInstance().m_slippiRegenerateReplays;
#endif

	if (!shouldSaveReplays)
	{
		return;
	}

	if (fileOption == "create" && !writeThreadRunning)
	{
		WARN_LOG(SLIPPI, "Creating file write thread...");
		writeThreadRunning = true;
		m_fileWriteThread = std::thread(&CEXISlippi::FileWriteThread, this);
	}

	if (!writeThreadRunning)
	{
		return;
	}

	std::vector<u8> payloadData;
	payloadData.insert(payloadData.end(), payload, payload + length);

	auto writeMsg = std::make_unique<WriteMessage>();
	writeMsg->data = payloadData;
	writeMsg->operation = fileOption;

	fileWriteQueue.Push(std::move(writeMsg));
}

void CEXISlippi::FileWriteThread(void)
{
	while (writeThreadRunning || !fileWriteQueue.Empty())
	{
		// Process all messages
		while (!fileWriteQueue.Empty())
		{
			writeToFile(std::move(fileWriteQueue.Front()));
			fileWriteQueue.Pop();

			Common::SleepCurrentThread(0);
		}

		Common::SleepCurrentThread(WRITE_FILE_SLEEP_TIME_MS);
	}
}

void CEXISlippi::writeToFile(std::unique_ptr<WriteMessage> msg)
{
	if (!msg)
	{
		ERROR_LOG(SLIPPI, "Unexpected error: write message is falsy.");
		return;
	}

	u8 *payload = msg->data.data();
	u32 length = (u32)msg->data.size();
	std::string fileOption = msg->operation;

	std::vector<u8> dataToWrite;
	if (fileOption == "create")
	{
		// If the game sends over option 1 that means a file should be created
		createNewFile();

		// Start ubjson file and prepare the "raw" element that game
		// data output will be dumped into. The size of the raw output will
		// be initialized to 0 until all of the data has been received
		std::vector<u8> headerBytes({'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0});
		dataToWrite.insert(dataToWrite.end(), headerBytes.begin(), headerBytes.end());

		// Used to keep track of how many bytes have been written to the file
		writtenByteCount = 0;

		// Used to track character usage (sheik/zelda)
		characterUsage.clear();

		// Reset lastFrame
		lastFrame = Slippi::GAME_FIRST_FRAME;

		// Get display names and connection codes from slippi netplay client
		if (slippi_netplay)
		{
			auto playerInfo = matchmaking->GetPlayerInfo();

			for (int i = 0; i < playerInfo.size(); i++)
			{
				slippi_names[i] = playerInfo[i].displayName;
				slippi_connect_codes[i] = playerInfo[i].connectCode;
			}
		}
	}

	// If no file, do nothing
	if (!m_file)
	{
		return;
	}

	// Update fields relevant to generating metadata at the end
	updateMetadataFields(payload, length);

	// Add the payload to data to write
	dataToWrite.insert(dataToWrite.end(), payload, payload + length);
	writtenByteCount += length;

	// If we are going to close the file, generate data to complete the UBJSON file
	if (fileOption == "close")
	{
		// This option indicates we are done sending over body
		std::vector<u8> closingBytes = generateMetadata();
		closingBytes.push_back('}');
		dataToWrite.insert(dataToWrite.end(), closingBytes.begin(), closingBytes.end());

		// Reset display names and connect codes retrieved from netplay client
		slippi_names.clear();
		slippi_connect_codes.clear();
	}

	// Write data to file
	bool result = m_file.WriteBytes(&dataToWrite[0], dataToWrite.size());
	if (!result)
	{
		ERROR_LOG(EXPANSIONINTERFACE, "Failed to write data to file.");
	}

	// If file should be closed, close it
	if (fileOption == "close")
	{
		// Write the number of bytes for the raw output
		std::vector<u8> sizeBytes = uint32ToVector(writtenByteCount);
		m_file.Seek(11, 0);
		m_file.WriteBytes(&sizeBytes[0], sizeBytes.size());

		// Close file
		closeFile();
	}
}

void CEXISlippi::createNewFile()
{
	if (m_file)
	{
		// If there's already a file open, close that one
		closeFile();
	}

#ifndef IS_PLAYBACK
	std::string dirpath = SConfig::GetInstance().m_strSlippiReplayDir;
	// in case the config value just gets lost somehow
	if (dirpath.empty())
	{
		SConfig::GetInstance().m_strSlippiReplayDir = File::GetHomeDirectory() + DIR_SEP + "Slippi";
		dirpath = SConfig::GetInstance().m_strSlippiReplayDir;
	}
#else
	std::string dirpath = SConfig::GetInstance().m_strSlippiRegenerateReplayDir;
	// in case the config value just gets lost somehow
	if (dirpath.empty())
	{
		SConfig::GetInstance().m_strSlippiRegenerateReplayDir =
		    File::GetHomeDirectory() + DIR_SEP + "Slippi" + DIR_SEP + "Regenerated";
		dirpath = SConfig::GetInstance().m_strSlippiRegenerateReplayDir;
	}
#endif

	// Remove a trailing / or \\ if the user managed to have that in their config
	char dirpathEnd = dirpath.back();
	if (dirpathEnd == '/' || dirpathEnd == '\\')
	{
		dirpath.pop_back();
	}

	// First, ensure that the root Slippi replay directory is created
	File::CreateFullPath(dirpath + "/");

#ifndef IS_PLAYBACK
	// Now we have a dir such as /home/Replays but we need to make one such
	// as /home/Replays/2020-06 if month categorization is enabled
	if (SConfig::GetInstance().m_slippiReplayMonthFolders)
	{
		dirpath.push_back('/');

		// Append YYYY-MM to the directory path
		uint8_t yearMonthStrLength = sizeof "2020-06";
		std::vector<char> yearMonthBuf(yearMonthStrLength);
		strftime(&yearMonthBuf[0], yearMonthStrLength, "%Y-%m", localtime(&gameStartTime));

		std::string yearMonth(&yearMonthBuf[0]);
		dirpath.append(yearMonth);

		// Ensure that the subfolder directory is created
		File::CreateDir(dirpath);
	}
#endif

	std::string filepath = dirpath + DIR_SEP + generateFileName();
	INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Creating new replay file %s", filepath.c_str());

#ifdef _WIN32
	m_file = File::IOFile(filepath, "wb", _SH_DENYWR);
#else
	m_file = File::IOFile(filepath, "wb");
#endif

	if (!m_file)
	{
		PanicAlertT("Could not create .slp replay file [%s].\n\n"
		            "The replay folder's path might be invalid, or you might "
		            "not have permission to write to it.\n\n"
		            "You can change the replay folder in Config > Slippi > "
		            "Slippi Replay Settings.",
		            filepath.c_str());
	}
}

std::string CEXISlippi::generateFileName()
{
	// Add game start time
	u8 dateTimeStrLength = sizeof "20171015T095717";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%Y%m%dT%H%M%S", localtime(&gameStartTime));

	std::string str(&dateTimeBuf[0]);
	return StringFromFormat("Game_%s.slp", str.c_str());
}

void CEXISlippi::closeFile()
{
	if (!m_file)
	{
		// If we have no file or payload is not game end, do nothing
		return;
	}

	// If this is the end of the game end payload, reset the file so that we create a new one
	m_file.Close();
	m_file = nullptr;
}

void CEXISlippi::prepareGameInfo(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	if (!m_current_game->AreSettingsLoaded())
	{
		m_read_queue.push_back(0);
		return;
	}

	// Return success code
	m_read_queue.push_back(1);

	// Prepare playback savestate payload
	playbackSavestatePayload.clear();
	appendWordToBuffer(&playbackSavestatePayload, 0); // This space will be used to set frame index
	int bkpPos = 0;
	while ((*(u32 *)(&payload[bkpPos * 8])) != 0)
	{
		bkpPos += 1;
	}
	playbackSavestatePayload.insert(playbackSavestatePayload.end(), payload, payload + (bkpPos * 8 + 4));

	Slippi::GameSettings *settings = m_current_game->GetSettings();

	// Start in Fast Forward if this is mirrored
	auto replayCommSettings = g_replayComm->getSettings();
	if (!g_playbackStatus->isHardFFW)
		g_playbackStatus->isHardFFW = replayCommSettings.mode == "mirror";

	g_playbackStatus->lastFFWFrame = INT_MIN;

	// Build a word containing the stage and the presence of the characters
	u32 randomSeed = settings->randomSeed;
	appendWordToBuffer(&m_read_queue, randomSeed);

	// This is kinda dumb but we need to handle the case where a player transforms
	// into sheik/zelda immediately. This info is not stored in the game info header
	// and so let's overwrite those values
	int player1Pos = 24; // This is the index of the first players character info
	std::array<u32, Slippi::GAME_INFO_HEADER_SIZE> gameInfoHeader = settings->header;
	for (int i = 0; i < 4; i++)
	{
		// check if this player is actually in the game
		bool playerExists = m_current_game->DoesPlayerExist(i);
		if (!playerExists)
		{
			continue;
		}

		// check if the player is playing sheik or zelda
		u8 externalCharId = settings->players[i].characterId;
		if (externalCharId != 0x12 && externalCharId != 0x13)
		{
			continue;
		}

		// this is the position in the array that this player's character info is stored
		int pos = player1Pos + (9 * i);

		// here we have determined the player is playing sheik or zelda...
		// at this point let's overwrite the player's character with the one
		// that they are playing
		gameInfoHeader[pos] &= 0x00FFFFFF;
		gameInfoHeader[pos] |= externalCharId << 24;
	}

	// Write entire header to game
	for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; i++)
	{
		appendWordToBuffer(&m_read_queue, gameInfoHeader[i]);
	}

	// Write UCF toggles
	std::array<u32, Slippi::UCF_TOGGLE_SIZE> ucfToggles = settings->ucfToggles;
	for (int i = 0; i < Slippi::UCF_TOGGLE_SIZE; i++)
	{
		appendWordToBuffer(&m_read_queue, ucfToggles[i]);
	}

	// Write nametags
	for (int i = 0; i < 4; i++)
	{
		auto player = settings->players[i];
		for (int j = 0; j < Slippi::NAMETAG_SIZE; j++)
		{
			appendHalfToBuffer(&m_read_queue, player.nametag[j]);
		}
	}

	// Write PAL byte
	m_read_queue.push_back(settings->isPAL);

	// Get replay version numbers
	auto replayVersion = m_current_game->GetVersion();
	auto majorVersion = replayVersion[0];
	auto minorVersion = replayVersion[1];

	// Write PS pre-load byte
	auto shouldPreloadPs = majorVersion > 1 || (majorVersion == 1 && minorVersion > 2);
	m_read_queue.push_back(shouldPreloadPs);

	// Write PS Frozen byte
	m_read_queue.push_back(settings->isFrozenPS);

	// Write should resync setting
	m_read_queue.push_back(replayCommSettings.shouldResync ? 1 : 0);

	// Write display names
	for (int i = 0; i < 4; i++)
	{
		auto displayName = settings->players[i].displayName;
		m_read_queue.insert(m_read_queue.end(), displayName.begin(), displayName.end());
	}

	// Return the size of the gecko code list
	prepareGeckoList();
	appendWordToBuffer(&m_read_queue, (u32)geckoList.size());

	// Initialize frame sequence index value for reading rollbacks
	frameSeqIdx = 0;

	if (replayCommSettings.rollbackDisplayMethod != "off")
	{
		// Prepare savestates
		availableSavestates.clear();
		activeSavestates.clear();

		// Prepare savestates for online play
		for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
		{
			availableSavestates.push_back(std::make_unique<SlippiSavestate>());
		}
	}
	else
	{
		// Prepare savestates
		availableSavestates.clear();
		activeSavestates.clear();

		// Add savestate for testing
		availableSavestates.push_back(std::make_unique<SlippiSavestate>());
	}

	// Reset playback frame to begining
	g_playbackStatus->currentPlaybackFrame = Slippi::GAME_FIRST_FRAME;

	// Initialize replay related threads if not viewing rollback versions of relays
	if (replayCommSettings.rollbackDisplayMethod == "off" &&
	    (replayCommSettings.mode == "normal" || replayCommSettings.mode == "queue"))
	{
		g_playbackStatus->startThreads();
	}
}

void CEXISlippi::prepareGeckoList()
{
	// This contains all of the codes required to play legacy replays (UCF, PAL, Frz Stadium)
	std::vector<u8> legacyCodelist = g_playbackStatus->getLegacyCodelist();

	// Assignment like this copies the values into a new map I think
	std::unordered_map<u32, bool> denylist = g_playbackStatus->getDenylist();

	auto replayCommSettings = g_replayComm->getSettings();

	// Some codes should only be denylisted when not displaying rollbacks, these are codes
	// that are required for things to not break when using Slippi savestates. Perhaps this
	// should be handled by actually applying these codes in the playback ASM instead? not sure
	auto should_deny = replayCommSettings.rollbackDisplayMethod == "off";
	denylist[0x8038add0] = should_deny; // Online/Core/PreventFileAlarms/PreventMusicAlarm.asm
	denylist[0x80023FFC] = should_deny; // Online/Core/PreventFileAlarms/MuteMusic.asm

	geckoList.clear();

	Slippi::GameSettings *settings = m_current_game->GetSettings();
	if (settings->geckoCodes.empty())
	{
		geckoList = legacyCodelist;
		return;
	}

	std::vector<u8> source = settings->geckoCodes;
	INFO_LOG(SLIPPI, "Booting codes with source size: %d", source.size());

	int idx = 0;
	while (idx < source.size())
	{
		u8 codeType = source[idx] & 0xFE;
		u32 address = source[idx] << 24 | source[idx + 1] << 16 | source[idx + 2] << 8 | source[idx + 3];
		address = (address & 0x01FFFFFF) | 0x80000000;

		u32 codeOffset = 8; // Default code offset. Most codes are this length
		switch (codeType)
		{
		case 0xC0:
		case 0xC2:
		{
			u32 lineCount = source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
			codeOffset = 8 + (lineCount * 8);
			break;
		}
		case 0x08:
			codeOffset = 16;
			break;
		case 0x06:
		{
			u32 byteLen = source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
			codeOffset = 8 + ((byteLen + 7) & 0xFFFFFFF8); // Round up to next 8 bytes and add the first 8 bytes
			break;
		}
		}

		idx += codeOffset;

		// If this address is denylisted, we don't add it to what we will send to game
		if (denylist[address])
			continue;

		INFO_LOG(SLIPPI, "Codetype [%x] Inserting section: %d - %d (%x, %d)", codeType, idx - codeOffset, idx, address,
		         codeOffset);

		// If not denylisted, add code to return vector
		geckoList.insert(geckoList.end(), source.begin() + (idx - codeOffset), source.begin() + idx);
	}

	// Add the termination sequence
	geckoList.insert(geckoList.end(), {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

void CEXISlippi::prepareCharacterFrameData(Slippi::FrameData *frame, u8 port, u8 isFollower)
{
	std::unordered_map<uint8_t, Slippi::PlayerFrameData> source;
	source = isFollower ? frame->followers : frame->players;

	// This must be updated if new data is added
	int characterDataLen = 52;

	// Check if player exists
	if (!source.count(port))
	{
		// If player does not exist, insert blank section
		m_read_queue.insert(m_read_queue.end(), characterDataLen, 0);
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = source[port];

	// log << frameIndex << "\t" << port << "\t" << data.locationX << "\t" << data.locationY << "\t" <<
	// data.animation
	// << "\n";

	// WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] [Player %d] Positions: %f | %f", frameIndex, port, data.locationX,
	// data.locationY);

	// Add all of the inputs in order
	appendWordToBuffer(&m_read_queue, data.randomSeed);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.joystickX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.joystickY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.cstickX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.cstickY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.trigger);
	appendWordToBuffer(&m_read_queue, data.buttons);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.locationX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.locationY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.facingDirection);
	appendWordToBuffer(&m_read_queue, (u32)data.animation);
	m_read_queue.push_back(data.joystickXRaw);
	m_read_queue.push_back(data.joystickYRaw);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.percent);
	m_read_queue.push_back(data.cstickXRaw);
	m_read_queue.push_back(data.cstickYRaw);
	// NOTE TO DEV: If you add data here, make sure to increase the size above
}

bool CEXISlippi::checkFrameFullyFetched(s32 frameIndex)
{
	auto doesFrameExist = m_current_game->DoesFrameExist(frameIndex);
	if (!doesFrameExist)
		return false;

	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);

	version::Semver200_version lastFinalizedVersion("3.7.0");
	version::Semver200_version currentVersion(m_current_game->GetVersionString());

	bool frameIsFinalized = true;
	if (currentVersion >= lastFinalizedVersion)
	{
		// If latest finalized frame should exist, check it as well. This will prevent us
		// from loading a non-committed frame when mirroring a rollback game
		frameIsFinalized = m_current_game->GetLastFinalizedFrame() >= frameIndex;
	}

	// This flag is set to true after a post frame update has been received. At that point
	// we know we have received all of the input data for the frame
	return frame->inputsFullyFetched && frameIsFinalized;
}

void CEXISlippi::prepareFrameData(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	// Parse input
	s32 frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	// If loading from queue, move on to the next replay if we have past endFrame
	auto watchSettings = g_replayComm->current;
#ifdef IS_PLAYBACK
	if (shouldOutput && !outputCurrentFrame && frameIndex >= watchSettings.startFrame)
		outputCurrentFrame = true;
	if (shouldOutput && outputCurrentFrame)
	{
		std::cout << "[CURRENT_FRAME] " << frameIndex << std::endl;
		if (frameIndex >= watchSettings.endFrame)
			outputCurrentFrame = false;
	}
#endif
	if (frameIndex > watchSettings.endFrame)
	{
		INFO_LOG(SLIPPI, "Killing game because we are past endFrame");
		m_read_queue.push_back(FRAME_RESP_TERMINATE);
		return;
	}

	// If a new replay should be played, terminate the current game
	auto isNewReplay = g_replayComm->isNewReplay();
	if (isNewReplay)
	{
		m_read_queue.push_back(FRAME_RESP_TERMINATE);
		return;
	}

	auto isProcessingComplete = m_current_game->IsProcessingComplete();
	// Wait until frame exists in our data before reading it. We also wait until
	// next frame has been found to ensure we have actually received all of the
	// data from this frame. Don't wait until next frame is processing is complete
	// (this is the last frame, in that case)
	auto isFrameFound = m_current_game->DoesFrameExist(frameIndex);
	g_playbackStatus->latestFrame = m_current_game->GetLatestIndex();
	auto isFrameComplete = checkFrameFullyFetched(frameIndex);
	auto isFrameReady = isFrameFound && (isProcessingComplete || isFrameComplete);

	// If there is a startFrame configured, manage the fast-forward flag
	if (watchSettings.startFrame > Slippi::GAME_FIRST_FRAME)
	{
		if (frameIndex < watchSettings.startFrame)
		{
			g_playbackStatus->setHardFFW(true);
		}
		else if (frameIndex == watchSettings.startFrame)
		{
			// TODO: This might disable fast forward on first frame when we dont want to?
			g_playbackStatus->setHardFFW(false);
		}
	}

	auto commSettings = g_replayComm->getSettings();
	if (commSettings.rollbackDisplayMethod == "normal")
	{
		auto nextFrame = m_current_game->GetFrameAt(frameSeqIdx);
		bool shouldHardFFW = nextFrame && nextFrame->frame <= g_playbackStatus->currentPlaybackFrame;
		g_playbackStatus->setHardFFW(shouldHardFFW);

		if (nextFrame)
		{
			// This feels jank but without this g_playbackStatus ends up getting updated to
			// a value beyond the frame that actually gets played causes too much FFW
			frameIndex = nextFrame->frame;
		}
	}

	// If RealTimeMode is enabled, let's trigger fast forwarding under certain conditions
	auto isFarBehind = g_playbackStatus->latestFrame - frameIndex > 2;
	auto isVeryFarBehind = g_playbackStatus->latestFrame - frameIndex > 25;
	if (isFarBehind && commSettings.mode == "mirror" && commSettings.isRealTimeMode)
	{
		g_playbackStatus->isSoftFFW = true;

		// Once isHardFFW has been turned on, do not turn it off with this condition, should
		// hard FFW to the latest point
		if (!g_playbackStatus->isHardFFW)
			g_playbackStatus->isHardFFW = isVeryFarBehind;
	}

	if (g_playbackStatus->latestFrame == frameIndex)
	{
		// The reason to disable fast forwarding here is in hopes
		// of disabling it on the last frame that we have actually received.
		// Doing this will allow the rendering logic to run to display the
		// last frame instead of the frame previous to fast forwarding.
		// Not sure if this fully works with partial frames
		g_playbackStatus->isSoftFFW = false;
		g_playbackStatus->setHardFFW(false);
	}

	bool shouldFFW = g_playbackStatus->shouldFFWFrame(frameIndex);
	u8 requestResultCode = shouldFFW ? FRAME_RESP_FASTFORWARD : FRAME_RESP_CONTINUE;
	if (!isFrameReady)
	{
		// If processing is complete, the game has terminated early. Tell our playback
		// to end the game as well.
		auto shouldTerminateGame = isProcessingComplete;
		requestResultCode = shouldTerminateGame ? FRAME_RESP_TERMINATE : FRAME_RESP_WAIT;
		m_read_queue.push_back(requestResultCode);

		// Disable fast forward here too... this shouldn't be necessary but better
		// safe than sorry I guess
		g_playbackStatus->isSoftFFW = false;
		g_playbackStatus->setHardFFW(false);

		if (requestResultCode == FRAME_RESP_TERMINATE)
		{
			ERROR_LOG(EXPANSIONINTERFACE, "Game should terminate on frame %d [%X]", frameIndex, frameIndex);
		}

		return;
	}

	u8 rollbackCode = 0; // 0 = not rollback, 1 = rollback, perhaps other options in the future?

	// Increment frame index if greater
	if (frameIndex > g_playbackStatus->currentPlaybackFrame || frameIndex != g_playbackStatus->currentPlaybackFrame)
	{
		g_playbackStatus->currentPlaybackFrame = frameIndex;
	}
	else if (commSettings.rollbackDisplayMethod != "off")
	{
		rollbackCode = 1;
	}

	// WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] Playback current behind by: %d frames.", frameIndex,
	//        g_playbackStatus->latestFrame - frameIndex);

	// Keep track of last FFW frame, used for soft FFW's
	if (shouldFFW)
	{
		WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] FFW frame, behind by: %d frames.", frameIndex,
		         g_playbackStatus->latestFrame - frameIndex);
		g_playbackStatus->lastFFWFrame = frameIndex;
	}

	// Return success code
	m_read_queue.push_back(requestResultCode);

	// Get frame
	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);
	if (commSettings.rollbackDisplayMethod != "off")
	{
		auto previousFrame = m_current_game->GetFrameAt(frameSeqIdx - 1);
		frame = m_current_game->GetFrameAt(frameSeqIdx);

		*(s32 *)(&playbackSavestatePayload[0]) = Common::swap32(frame->frame);

		if (previousFrame && frame->frame <= previousFrame->frame)
		{
			// Here we should load a savestate
			handleLoadSavestate(&playbackSavestatePayload[0]);
		}

		// Here we should save a savestate
		handleCaptureSavestate(&playbackSavestatePayload[0]);

		frameSeqIdx += 1;
	}

	// For normal replays, modify slippi seek/playback data as needed
	// TODO: maybe handle other modes too?
	if (commSettings.mode == "normal" || commSettings.mode == "queue")
	{
		g_playbackStatus->prepareSlippiPlayback(frame->frame);
	}

	// Push RB code
	m_read_queue.push_back(rollbackCode);

	// Add frame rng seed to be restored at priority 0
	u8 rngResult = frame->randomSeedExists ? 1 : 0;
	m_read_queue.push_back(rngResult);
	appendWordToBuffer(&m_read_queue, *(u32 *)&frame->randomSeed);

	// Add frame data for every character
	for (u8 port = 0; port < 4; port++)
	{
		prepareCharacterFrameData(frame, port, 0);
		prepareCharacterFrameData(frame, port, 1);
	}
}

void CEXISlippi::prepareIsStockSteal(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	// Parse args
	s32 frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	u8 playerIndex = payload[4];

	// I'm not sure checking for the frame should be necessary. Theoretically this
	// should get called after the frame request so the frame should already exist
	auto isFrameFound = m_current_game->DoesFrameExist(frameIndex);
	if (!isFrameFound)
	{
		m_read_queue.push_back(0);
		return;
	}

	// Load the data from this frame into the read buffer
	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);
	auto players = frame->players;

	u8 playerIsBack = players.count(playerIndex) ? 1 : 0;
	m_read_queue.push_back(playerIsBack);
}

void CEXISlippi::prepareIsFileReady()
{
	m_read_queue.clear();

	// Hides frame index message on waiting for game screen
	OSD::AddTypedMessage(OSD::MessageType::FrameIndex, "", 0, OSD::Color::CYAN);

	auto isNewReplay = g_replayComm->isNewReplay();
	if (!isNewReplay)
	{
		g_replayComm->nextReplay();
		m_read_queue.push_back(0);
		return;
	}

	// Attempt to load game if there is a new replay file
	// this can come pack falsy if the replay file does not exist
	m_current_game = g_replayComm->loadGame();
	if (!m_current_game)
	{
		// Do not start if replay file doesn't exist
		// TODO: maybe display error message?
		INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file does not exist?");
		m_read_queue.push_back(0);
		return;
	}
#ifdef IS_PLAYBACK
	if (shouldOutput)
	{
		auto lastFrame = m_current_game->GetLatestIndex();
		auto gameEndMethod = m_current_game->GetGameEndMethod();
		auto watchSettings = g_replayComm->current;
		auto replayCommSettings = g_replayComm->getSettings();
		std::cout << "[FILE_PATH] " << watchSettings.path << std::endl;
		if (gameEndMethod == 0 || gameEndMethod == 7)
			std::cout << "[LRAS]" << std::endl;
		std::cout << "[PLAYBACK_START_FRAME] " << watchSettings.startFrame << std::endl;
		std::cout << "[GAME_END_FRAME] " << lastFrame << std::endl;
		std::cout << "[PLAYBACK_END_FRAME] " << watchSettings.endFrame << std::endl;
	}
#endif
	INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file loaded successfully!?");

	// Clear playback control related vars
	g_playbackStatus->resetPlayback();

	// Start the playback!
	m_read_queue.push_back(1);
}

// The original reason for this was to avoid crashes when people disconnected during CSS/VSS Screens, causing that
// slippi_netplay got set to null on it's own thread and then the instance of the ExiDevice would crash while performing
// a method that was that used it.
// Maybe someone smart can fix that logic instead of this monkey patch.
bool CEXISlippi::isDisconnected()
{
	if (!slippi_netplay)
		return true;

	auto status = slippi_netplay->GetSlippiConnectStatus();
	return status != SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;
}

// Open our router towards everyone trying to watch this match.
//
// ⚠ Repeatedly, not once. A NAT mapping made by one packet expires in
// anything from twenty seconds upwards, a watcher can arrive at any point in a
// match, and the punch has to already have happened when they do. Once a
// second is far below the shortest timeout worth designing for and is two
// packets a second in the worst case.
void CEXISlippi::punchAtWatchers()
{
	if (!slippi_netplay || isWatching())
		return;

	u64 now = Common::Timer::GetTimeMs();
	if (now - last_punch_ms < 1000)
		return;
	last_punch_ms = now;

	Rooms::State rs = Rooms::Latest();
	for (const auto &a : rs.punch)
		slippi_netplay->PunchTo(a);

	// Said when the list CHANGES, which is when somebody starts or stops
	// watching - not every second.
	static size_t said = (size_t)-1;
	if (rs.punch.size() != said)
	{
		said = rs.punch.size();
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] punching a hole for %d watcher(s)", (int)said);
	}
}

void CEXISlippi::handleOnlineInputs(u8 *payload)
{
	punchAtWatchers();

	m_read_queue.clear();

	s32 frame = Common::swap32(&payload[0]);
	s32 finalizedFrame = Common::swap32(&payload[4]);
	u32 finalizedFrameChecksum = Common::swap32(&payload[8]);
	u8 delay = payload[12];
	u8 *inputs = &payload[13];

	if (frame == 1)
	{
		availableSavestates.clear();
		activeSavestates.clear();

		// Prepare savestates for online play
		for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
		{
			availableSavestates.push_back(std::make_unique<SlippiSavestate>());
		}

		// Reset per-player stall counters
		for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
		{
			stallFrameCounts[i] = 0;
		}

		lastIntervalTimeUs = 0;
		perfDebt = 0;

		// Reset skip variables
		framesToSkip = 0;
		isCurrentlySkipping = false;

		// Reset advance stuff
		framesToAdvance = 0;
		isCurrentlyAdvancing = false;
		fallBehindCounter = 0;
		fallFarBehindCounter = 0;

		// Reset character selections such that they are cleared for next game
		localSelections.Reset();
		if (slippi_netplay)
			slippi_netplay->StartSlippiGame();
	}

	if (isDisconnected())
	{
		// Both clients reach this path when a poor-performance termination fires: the initiating
		// side set the reason locally, the other side received it over the disconnect. Show the
		// message on both ends rather than only where the debt happened to cross the threshold first.
		if (slippi_netplay &&
		    slippi_netplay->GetDisconnectReason() == SlippiNetplayClient::SlippiDisconnectReason::POOR_PERFORMANCE)
		{
			OSD::AddTypedMessage(
			    OSD::MessageType::PoorPerformanceTermination,
			    "\nThe match has been terminated due to poor network quality.\nIf you see this message in most "
			    "of your matches, you probably shouldn't be playing ranked.",
			    15000, OSD::Color::RED);
		}

		m_read_queue.push_back(3); // Indicate we disconnected
		return;
	}

	// A watcher only consumes. Its netplay client is a stand-in with no peers -
	// nothing to trim, nobody to send to, and no connection to judge. Everything
	// about this frame comes from the timeline.
	if (isWatching())
	{
		prepareOpponentInputs(frame, watch_client->LatestFrame() < frame);
		return;
	}

	// Drop inputs that we no longer need (inputs older than the finalized frame passed in)
	slippi_netplay->DropOldRemoteInputs(finalizedFrame);

	bool shouldSkip = shouldSkipOnlineFrame(frame, finalizedFrame);
	if (shouldSkip)
	{
		// Send inputs that have not yet been acked
		slippi_netplay->SendSlippiPad(nullptr);
	}
	else
	{
		// Consider disconnecting from a match if performance is poor
		handlePoorMatchPerformance(frame);

		// Send the input for this frame along with everything that has yet to be acked
		handleSendInputs(frame, delay, finalizedFrame, finalizedFrameChecksum, inputs);
	}

	prepareOpponentInputs(frame, shouldSkip);
}

void CEXISlippi::handlePoorMatchPerformance(s32 frame)
{
	// Only handle poor match performance in ranked. In other modes players can just manually leave. Plus
	// ranked is mostly where it matters
	if (lastSearch.mode != SlippiMatchmaking::OnlinePlayMode::RANKED)
		return;

	if (!slippi_netplay)
		return;

	u64 intervalFrames = 150; // Check every 2.5 seconds
	u64 frameTimeUs = 16683;

	// Skip the first 50 frames of the game and check timing info every interval
	if ((frame + (intervalFrames - 50)) % intervalFrames != 0)
		return;

	// The modifier increases if we've been in a game for a minute. At that point, make it a little harder
	// for the debt to accumulate to failure
	double modifier = frame > 3600 ? 1.15 : 1.0;

	auto curTimeUs = Common::Timer::GetTimeUs();

	// Iniitalize the first instance
	if (lastIntervalTimeUs == 0)
	{
		lastIntervalTimeUs = curTimeUs;
		// Discard ping samples collected before this point so the first processed interval's
		// average covers exactly one interval instead of everything since connection start
		slippi_netplay->GetAndResetAvgPingMs();
		return; // We will start processing the next time
	}

	auto expectedTimeUs = frameTimeUs * intervalFrames;
	double ratio = static_cast<double>(curTimeUs - lastIntervalTimeUs) / static_cast<double>(expectedTimeUs);
	lastIntervalTimeUs = curTimeUs;

	// Leaky accumulator: each interval adds "debt" proportional to how far over the expected duration we ran,
	// and healthy intervals pay it back down. We only terminate once enough debt builds up, so a single hitch
	// (local or otherwise) is survivable but sustained degradation isn't. The decay keeps this independent of
	// match length: scattered blips drain away before they can accumulate to the termination threshold.
	s32 terminateThreshold = 30;
	s32 speedDebt;
	if (ratio >= 1.0 + 0.75 * modifier)
		speedDebt = 15; // Severe
	else if (ratio >= 1.0 + 0.5 * modifier)
		speedDebt = 8; // Bad
	else if (ratio >= 1.0 + 0.1 * modifier)
		speedDebt = 4; // Mild
	else
		speedDebt = -1; // Healthy interval, pay down accumulated debt

	// High ping feeds the same accumulator. Averaging over the interval (~150 samples) means a brief
	// spike gets diluted while sustained high ping keeps every interval elevated. The mild tier starts
	// just above the 90ms "playable" line; against the -1 decay, a connection oscillating around that
	// line net-accumulates once it spends over a fifth of its time above it, and a constantly-mild
	// connection terminates in 8 intervals (~20s). An average of 0 means no acks arrived this
	// interval; the speed ratio handles that case.
	double avgPingMs = slippi_netplay->GetAndResetAvgPingMs();
	s32 pingDebt;
	if (avgPingMs >= 200 * modifier)
		pingDebt = 15; // Severe
	else if (avgPingMs >= 120 * modifier)
		pingDebt = 8; // Bad
	else if (avgPingMs >= 90 * modifier)
		pingDebt = 4; // Mild
	else
		pingDebt = -1; // Healthy interval, pay down accumulated debt

	// Take the worse of the two signals rather than summing: a network problem often inflates both
	// (waiting on remote inputs stretches the interval and delays acks), so summing would double-count
	// a single underlying cause
	s32 debt = std::max(speedDebt, pingDebt);

	perfDebt = std::max(0, perfDebt + debt);
	INFO_LOG(SLIPPI_ONLINE,
	         "Modifying performance debt by %d (speed: %d, ping: %d, avgPingMs: %.1f). Currently at: %d/%d", debt,
	         speedDebt, pingDebt, avgPingMs, perfDebt, terminateThreshold);
	if (perfDebt >= terminateThreshold)
	{
		// Tell the server about the poor performance, then drop all remote players with a
		// POOR_PERFORMANCE reason. Flipping the connection to DISCONNECTED lets the existing
		// disconnect-detection path end the game naturally (next handleOnlineInputs sees
		// isDisconnected()), same as a real disconnect. The reason rides the disconnect to the
		// peer so both clients surface the OSD message from the shared disconnect path below.
		slprs_exi_device_report_match_status(slprs_exi_device_ptr, recentMmResult.id.c_str(), "poor_performance", true);
		slippi_netplay->ForceDisconnect(SlippiNetplayClient::SlippiDisconnectReason::POOR_PERFORMANCE);
		ERROR_LOG(SLIPPI_ONLINE, "Match terminated due to poor performance. %d/%d", perfDebt, terminateThreshold);
	}
}

bool CEXISlippi::shouldSkipOnlineFrame(s32 frame, s32 finalizedFrame)
{
	auto status = slippi_netplay->GetSlippiConnectStatus();
	bool connectionFailed = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
	bool connectionDisconnected = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_DISCONNECTED;
	if (connectionFailed || connectionDisconnected)
	{
		// If connection failed just continue the game
		return false;
	}

	// Check each active remote player individually. If any single player is short on
	// new inputs we still skip the frame, but we only force-disconnect the specific
	// player(s) whose stall counter exceeds the threshold. In a 1v1 this collapses to
	// the previous behavior (sole opponent gets dropped → connectionDisconnected path
	// fires next frame). In multiplayer, the despawn/continue path takes over.
	// ROLLBACK_MAX_FRAMES is our look-ahead limit: see the prior comment block in git
	// history for the savestate math.
	bool anyPlayerNeedsInputs = false;
	u8 remotePlayerCount = matchmaking->RemotePlayerCount();
	for (u8 i = 0; i < remotePlayerCount; i++)
	{
		auto pad = slippi_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
		if (pad->isDisconnected)
		{
			stallFrameCounts[i] = 0;
			continue;
		}

		s32 latestRemoteFrame = pad->latestFrame;
		bool hasEnoughNewInputs = latestRemoteFrame - finalizedFrame >= (frame - finalizedFrame - ROLLBACK_MAX_FRAMES);
		if (hasEnoughNewInputs)
		{
			stallFrameCounts[i] = 0;
			continue;
		}

		stallFrameCounts[i]++;
		anyPlayerNeedsInputs = true;

		if (stallFrameCounts[i] > 60 * 7)
		{
			WARN_LOG(SLIPPI_ONLINE, "Force-disconnecting player %d after 7s stall (frame: %d | latest: %d)",
			         pad->playerIdx, frame, latestRemoteFrame);
			slippi_netplay->ForceDisconnectPlayer(pad->playerIdx);
			stallFrameCounts[i] = 0;
			continue;
		}

		WARN_LOG(SLIPPI_ONLINE,
		         "Halting for one frame due to rollback limit (frame: %d | latest: %d | finalized: %d | player: %d)...",
		         frame, latestRemoteFrame, finalizedFrame, pad->playerIdx);
	}

	if (anyPlayerNeedsInputs)
		return true;

	s32 frameTime = 16683;
	s32 t1 = 10000;
	s32 t2 = (2 * frameTime) + t1;

	// 8/8/23: Removed the halting time sync logic in favor of emulation speed. Hopefully less halts means
	// less dropped inputs. We will only do it at the start of the game to sync everything up
	// 9/18/23: Brought back frame skips when behind in the other location

	// Only skip once for a given frame because our time detection method doesn't take into consideration
	// waiting for a frame. Also it's less jarring and it happens often enough that it will smoothly
	// get to the right place
	auto isTimeSyncFrame = frame % SLIPPI_ONLINE_LOCKSTEP_INTERVAL; // Only time sync every 30 frames
	if (isTimeSyncFrame == 0 && !isCurrentlySkipping && frame <= 120)
	{
		auto offsetUs = slippi_netplay->CalcTimeOffsetUs();
		INFO_LOG(SLIPPI_ONLINE, "[Frame %d] Offset for skip is: %d us", frame, offsetUs);

		// At the start of the game, let's make sure to sync perfectly, but after that let the slow instance
		// try to do more work before we stall

		// The decision to skip a frame only happens when we are already pretty far off ahead. The hope is
		// that this won't really be used much because the frame advance of the slow client along with
		// dynamic emulation speed will pick up the difference most of the time. But at some point it's
		// probably better to slow down...
		if (offsetUs > (frame <= 120 ? t1 : t2))
		{
			isCurrentlySkipping = true;

			int maxSkipFrames = frame <= 120 ? 5 : 1; // On early frames, support skipping more frames
			framesToSkip = ((offsetUs - t1) / frameTime) + 1;
			framesToSkip = framesToSkip > maxSkipFrames ? maxSkipFrames : framesToSkip; // Only skip 5 frames max

			WARN_LOG(SLIPPI_ONLINE, "Halting on frame %d due to time sync. Offset: %d us. Frames: %d...", frame,
			         offsetUs, framesToSkip);
		}
	}

	// Handle the skipped frames
	if (framesToSkip > 0)
	{
		// If ahead by 60% of a frame, stall. I opted to use 60% instead of half a frame
		// because I was worried about two systems continuously stalling for each other
		framesToSkip = framesToSkip - 1;
		return true;
	}

	isCurrentlySkipping = false;

	return false;
}

bool CEXISlippi::shouldAdvanceOnlineFrame(s32 frame)
{
	// A watcher has no opponent to stay level with - it chases the timeline, and
	// starts behind by however long the match has been going.
	//
	// ⚠ Two halves and it needs BOTH. The throttler off lets the emulator run
	// faster than sixty frames a second; RESP_ADVANCE (this returning true) makes
	// Melee loop its engine an extra time, so two frames are simulated and one is
	// drawn. Either alone does nothing useful.
	//
	// ⚠ And it has to be RATIONED. Returned on every poll the frame counter
	// races away, the engine never gets to loop, and the watcher lands on the
	// live frame holding a state that was never simulated - nobody damaged, the
	// clock minutes behind. One frame in two is double speed.
	if (isWatching())
	{
		s32 behind = watch_client->LatestFrame() - frame;
		setCatchUpSpeed(behind > 10);
		if ((frame % 60) == 0)
			WARN_LOG(SLIPPI_ONLINE, "[Watch] frame %d, live %d, %d behind, %s", frame, watch_client->LatestFrame(),
			         behind, behind > 10 ? "catching up" : "level");
		return behind > 10 && (frame % 2) == 0;
	}
	setCatchUpSpeed(false);

	// If the opponent is a bot running ahead to give us more inputs, we should
	// just keep going at our own pace rather than trying to catch up.
	if (opponentRunahead())
		return false;

	// Logic below is used to test frame advance by forcing it more often
	// SConfig::GetInstance().m_EmulationSpeed = 0.5f;
	// if (frame > 120 && frame % 10 < 3)
	//{
	//	Common::SleepCurrentThread(1); // Sleep to try to let inputs come in to make late rollbacks more likely
	//	return true;
	//}

	// return false;
	// return frame % 2 == 0;

	// Return true if we are over 60% of a frame behind our opponent. We limit how often this happens
	// to get a reliable average to act on. We will allow advancing up to 5 frames (spread out) over
	// the 30 frame period. This makes the game feel relatively smooth still
	auto isTimeSyncFrame = (frame % SLIPPI_ONLINE_LOCKSTEP_INTERVAL) == 0; // Only time sync every 30 frames
	if (isTimeSyncFrame)
	{
		auto offsetUs = slippi_netplay->CalcTimeOffsetUs();

		// Dynamically adjust emulation speed in order to fine-tune time sync to reduce one sided rollbacks even more
		// Modify emulation speed up to a max of 1% at 3 frames offset or more. Don't slow down the front instance as
		// much because we want to prioritize performance for the fast PC
		float deviation = 0;
		float maxSlowDownAmount = 0.005f;
		float maxSpeedUpAmount = 0.01f;
		int slowDownFrameWindow = 3;
		int speedUpFrameWindow = 3;
		if (offsetUs > -250 && offsetUs < 8000)
		{
			// Do nothing, leave deviation at 0 for 100% emulation speed when ahead by 8 ms or less
		}
		else if (offsetUs < 0)
		{
			// Here we are behind, so let's speed up our instance
			float frameWindowMultiplier = std::min(-offsetUs / (speedUpFrameWindow * 16683.0f), 1.0f);
			deviation = frameWindowMultiplier * maxSpeedUpAmount;
		}
		else
		{
			// Here we are ahead, so let's slow down our instance
			float frameWindowMultiplier = std::min(offsetUs / (slowDownFrameWindow * 16683.0f), 1.0f);
			deviation = frameWindowMultiplier * -maxSlowDownAmount;
		}

		auto dynamicEmulationSpeed = 1.0f + deviation;
		SConfig::GetInstance().m_EmulationSpeed = dynamicEmulationSpeed;
		// SConfig::GetInstance().m_EmulationSpeed = 0.97f; // used for testing

		INFO_LOG(SLIPPI_ONLINE, "[Frame %d] Offset for advance is: %d us. New speed: %.2f%%", frame, offsetUs,
		         dynamicEmulationSpeed * 100.0f);

		s32 frameTime = 16683;
		s32 t1 = 10000;
		s32 t2 = frameTime + t1;

		// Count the number of times we're below a threshold we should easily be able to clear. This is checked twice
		// per second.
		fallBehindCounter += offsetUs < -t1 ? 1 : 0;
		fallFarBehindCounter += offsetUs < -t2 ? 1 : 0;

		bool isSlow = (offsetUs < -t1 && fallBehindCounter > 50) || (offsetUs < -t2 && fallFarBehindCounter > 15);
		if (isSlow && matchmaking->RemotePlayerCount() == 1 &&
		    lastSearch.mode != SlippiMatchmaking::OnlinePlayMode::RANKED)
		{
			// Only show this in 1v1. With more peers, CalcTimeOffsetUs returns the min across peers, which biases
			// negative as the peer count grows and false-positives this warning. The message text ("if this appears
			// with most opponents") also only makes sense in 1v1. Don't show in ranked because we have the poor
			// match logic running there which has its own message
			OSD::AddTypedMessage(
			    OSD::MessageType::PerformanceWarning,
			    "\nPossible poor match performance detected.\nIf this message appears with most opponents, your "
			    "computer or network is likely impacting match performance for the other players.",
			    10000, OSD::Color::RED);
		}

		if (offsetUs < -t2 && !isCurrentlyAdvancing)
		{
			isCurrentlyAdvancing = true;

			// On early frames, don't advance any frames. Let the stalling logic handle the initial sync
			int maxAdvFrames = frame > 120 ? 3 : 0;
			framesToAdvance = ((-offsetUs - t1) / frameTime) + 1;
			framesToAdvance = framesToAdvance > maxAdvFrames ? maxAdvFrames : framesToAdvance;

			WARN_LOG(SLIPPI_ONLINE, "Advancing on frame %d due to time sync. Offset: %d us. Frames: %d...", frame,
			         offsetUs, framesToAdvance);
		}
	}

	// Handle the skipped frames
	if (framesToAdvance > 0)
	{
		// Only advance once every 5 frames in an attempt to make the speed up feel smoother
		if (frame % 5 != 0)
		{
			return false;
		}

		framesToAdvance = framesToAdvance - 1;
		return true;
	}

	isCurrentlyAdvancing = false;
	return false;
}

void CEXISlippi::handleSendInputs(s32 frame, u8 delay, s32 checksumFrame, u32 checksum, u8 *inputs)
{
	// On the first frame sent, we need to queue up empty dummy pads for as many
	//	frames as we have delay
	if (frame == 1)
	{
		for (int i = 1; i <= delay; i++)
		{
			auto empty = std::make_unique<SlippiPad>(i);
			slippi_netplay->SendSlippiPad(std::move(empty));
		}
	}

	auto pad = std::make_unique<SlippiPad>(frame + delay, checksumFrame, checksum, inputs);

	slippi_netplay->SendSlippiPad(std::move(pad));
}

bool CEXISlippi::opponentRunahead()
{
	// Bot players might be running ahead to "donate" their delay frames to us.

	// Only registered bot accounts are allowed to do this.
	auto player_info = matchmaking->GetPlayerInfo();
	for (int i = 0; i < player_info.size(); i++)
	{
		if (i == matchmaking->LocalPlayerIndex())
			continue;

		if (!player_info[i].isBot)
			return false;
	}

	return true;
}

void CEXISlippi::prepareOpponentInputs(s32 frame, bool shouldSkip)
{
	m_read_queue.clear();

	u8 frameResult = 1; // Indicates to continue frame

	// A watcher's stand-in netplay client has no peers and would report itself
	// disconnected, which ends the game. What keeps it going is the timeline,
	// and the timeline is what shouldSkip already reflects.
	auto state = isWatching() ? SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED
	                          : slippi_netplay->GetSlippiConnectStatus();
	if (shouldSkip)
	{
		// Event though we are skipping an input, we still want to prepare the opponent inputs because
		// in the case where we get a stall on an advance frame, we need to keep the RXB inputs populated
		// for when the frame inputs are requested on a rollback
		frameResult = 2;
	}
	else if (state != SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED)
	{
		frameResult = 3; // Indicates we have disconnected
	}
	else if (shouldAdvanceOnlineFrame(frame))
	{
		frameResult = 4;
	}

	m_read_queue.push_back(frameResult); // Write out the control message value

	// Watching: the two playing are BOTH remote, so this is two rather than the
	// one a player has. See SlippiWatchClient::WATCHER_PORT.
	bool watching = isWatching();
	u8 remotePlayerCount = watching ? 2 : matchmaking->RemotePlayerCount();
	m_read_queue.push_back(remotePlayerCount); // Indicate the number of remote players

	std::unique_ptr<SlippiRemotePadOutput> results[SLIPPI_REMOTE_PLAYER_MAX];

	s32 latestFrameFromOpps = Slippi::GAME_FIRST_FRAME - 1;
	u32 lastChecksumFrame = 0;
	u32 lastChecksum = 0;
	for (int i = 0; i < remotePlayerCount; i++)
	{
		// Remote i is port i for a watcher, because the watcher sits at port 2
		// and the netplay client's mapping skips its own index.
		results[i] = watching ? WatchRemotePad(watch_client.get(), frame, (u8)i)
		                      : slippi_netplay->GetSlippiRemotePad(i, ROLLBACK_MAX_FRAMES);
		if (results[i]->isDisconnected)
		{
			continue;
		}
		// results[i] = slippi_netplay->GetFakePadOutput(frame);

		if (results[i]->latestFrame > latestFrameFromOpps)
		{
			lastChecksumFrame = static_cast<u32>(results[i]->checksumFrame);
			lastChecksum = results[i]->checksum;
			latestFrameFromOpps = results[i]->latestFrame;
		}
	}

	// Determine whether each remote fighter should be despawned due to disconnect. This needs
	// to be computed before the loop below overwrites the disconnected players' latestFrame.
	// We want the signal to be synchronized across clients, so we wait until
	// (2*ROLLBACK_MAX_FRAMES + 2) frames have passed since the last input we received from the
	// disconnected player, then advance to the next frame that is a multiple of
	// SLIPPI_DESPAWN_FRAME_INTERVAL. This gives us a window where slightly different last
	// received frames might still work out to the same despawn frame
	constexpr s32 SLIPPI_DESPAWN_FRAME_INTERVAL = 30;
	u8 shouldDespawn[SLIPPI_REMOTE_PLAYER_MAX] = {0, 0, 0};
	for (int i = 0; i < remotePlayerCount; i++)
	{
		if (!results[i]->isDisconnected)
			continue;

		s32 lastInputFrame = results[i]->latestFrame;
		s32 thresholdFrame = lastInputFrame + 2 * ROLLBACK_MAX_FRAMES + 2;

		// Round up to the next frame that is a multiple of the despawn interval
		s32 despawnFrame = ((thresholdFrame + SLIPPI_DESPAWN_FRAME_INTERVAL - 1) / SLIPPI_DESPAWN_FRAME_INTERVAL) *
		                   SLIPPI_DESPAWN_FRAME_INTERVAL;

		if (frame >= despawnFrame)
			shouldDespawn[i] = 1;
	}

	for (int i = 0; i < remotePlayerCount; i++)
	{
		if (!results[i]->isDisconnected)
		{
			// INFO_LOG(SLIPPI_ONLINE, "Sending checksum values: [%d] %08x", results[i]->checksumFrame,
			// results[i]->checksum);
			appendWordToBuffer(&m_read_queue, static_cast<u32>(results[i]->checksumFrame));
			appendWordToBuffer(&m_read_queue, results[i]->checksum);
			continue;
		}

		// This is sorta jank but we loop again to overwrite values on any disconnected pads to prevent checksum
		// issues and prevent stalling due to old pad data. We are essentially "tricking" the ASM side here
		// and likely a better solution would be for the ASM side to know who is disconnected and handle it accordingly
		results[i]->latestFrame = latestFrameFromOpps;
		appendWordToBuffer(&m_read_queue, lastChecksumFrame);
		appendWordToBuffer(&m_read_queue, lastChecksum);
	}

	for (int i = remotePlayerCount; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
	{
		// Send dummy data for unused players
		appendWordToBuffer(&m_read_queue, 0);
		appendWordToBuffer(&m_read_queue, 0);
	}

	int offset[SLIPPI_REMOTE_PLAYER_MAX];
	// INFO_LOG(SLIPPI_ONLINE, "Preparing pad data for frame %d", frame);

	int32_t latestFrameRead[SLIPPI_REMOTE_PLAYER_MAX]{};

	// Get pad data for each remote player and write each of their latest frame nums to the buf
	for (int i = 0; i < remotePlayerCount; i++)
	{
		// determine offset from which to copy data
		offset[i] = (results[i]->latestFrame - frame) * SLIPPI_PAD_FULL_SIZE;
		offset[i] = offset[i] < 0 ? 0 : offset[i];

		// add latest frame we are transfering to begining of return buf
		int32_t latestFrame = results[i]->latestFrame;
		if (latestFrame > frame)
			latestFrame = frame;
		latestFrameRead[i] = latestFrame;
		appendWordToBuffer(&m_read_queue, static_cast<u32>(latestFrame));
		// INFO_LOG(SLIPPI_ONLINE, "Sending frame num %d for pIdx %d (offset: %d)", latestFrame, i, offset[i]);
	}
	// Send the current frame for any unused player slots.
	for (int i = remotePlayerCount; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
	{
		latestFrameRead[i] = frame;
		appendWordToBuffer(&m_read_queue, static_cast<u32>(frame));
	}

	s32 *val = std::min_element(std::begin(latestFrameRead), std::end(latestFrameRead));
	appendWordToBuffer(&m_read_queue, static_cast<u32>(*val));

	// copy pad data over
	for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
	{
		std::vector<u8> tx;

		// Get pad data if this remote player exists
		if (i < remotePlayerCount && offset[i] < results[i]->data.size())
		{
			auto txStart = results[i]->data.begin() + offset[i];
			auto txEnd = results[i]->data.end();
			tx.insert(tx.end(), txStart, txEnd);
		}

		tx.resize(SLIPPI_PAD_FULL_SIZE * ROLLBACK_MAX_FRAMES, 0);

		m_read_queue.insert(m_read_queue.end(), tx.begin(), tx.end());
	}

	// Append the per-remote-player should-despawn flags
	for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
	{
		m_read_queue.push_back(shouldDespawn[i]);
	}

	// ERROR_LOG(SLIPPI_ONLINE, "EXI: [%d] %X %X %X %X %X %X %X %X", latestFrame, m_read_queue[5], m_read_queue[6],
	// m_read_queue[7], m_read_queue[8], m_read_queue[9], m_read_queue[10], m_read_queue[11], m_read_queue[12]);
}

void CEXISlippi::handleCaptureSavestate(u8 *payload)
{
#ifndef IS_PLAYBACK
	if (isDisconnected())
		return;
#endif

	s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	// u64 startTime = Common::Timer::GetTimeUs();

	// Grab an available savestate
	std::unique_ptr<SlippiSavestate> ss;
	if (!availableSavestates.empty())
	{
		ss = std::move(availableSavestates.back());
		availableSavestates.pop_back();
	}
	else
	{
		// If there were no available savestates, use the oldest one
		auto it = activeSavestates.begin();
		ss = std::move(it->second);
		activeSavestates.erase(it->first);
	}

	// If there is already a savestate for this frame, remove it and add it to available
	if (activeSavestates.count(frame))
	{
		availableSavestates.push_back(std::move(activeSavestates[frame]));
		activeSavestates.erase(frame);
	}

	ss->Capture();
	activeSavestates[frame] = std::move(ss);

	// u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
	// INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Captured savestate for frame %d in: %f ms", frame,
	//         ((double)timeDiff) / 1000);
}

void CEXISlippi::handleLoadSavestate(u8 *payload)
{
	s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	u32 *preserveArr = (u32 *)(&payload[4]);

	if (!activeSavestates.count(frame))
	{
		// This savestate does not exist... uhhh? What do we do?
		ERROR_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Savestate for frame %d does not exist.", frame);
		return;
	}

	// u64 startTime = Common::Timer::GetTimeUs();

	// Fetch preservation blocks
	std::vector<SlippiSavestate::PreserveBlock> blocks;

	// Get preservation blocks
	int idx = 0;
	while (Common::swap32(preserveArr[idx]) != 0)
	{
		SlippiSavestate::PreserveBlock p = {Common::swap32(preserveArr[idx]), Common::swap32(preserveArr[idx + 1])};
		blocks.push_back(p);
		idx += 2;
	}

	// Load savestate
	activeSavestates[frame]->Load(blocks);

	// Move all active savestates to available
	for (auto it = activeSavestates.begin(); it != activeSavestates.end(); ++it)
	{
		availableSavestates.push_back(std::move(it->second));
	}

	activeSavestates.clear();

	// u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
	// INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Loaded savestate for frame %d in: %f ms", frame, ((double)timeDiff) /
	// 1000);
}

void CEXISlippi::startFindMatch(u8 *payload)
{
	SlippiMatchmaking::MatchSearchSettings search;
	search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];

	std::string shiftJisCode;
	shiftJisCode.insert(shiftJisCode.begin(), &payload[1], &payload[1] + 18);
	shiftJisCode.erase(std::find(shiftJisCode.begin(), shiftJisCode.end(), 0x00), shiftJisCode.end());

	// Log the direct code to file.
	if (search.mode == SlippiMatchmaking::DIRECT)
	{
		// Make sure to convert to UTF8, otherwise json library will fail when
		// calling dump().
		std::string utf8Code = SHIFTJISToUTF8(shiftJisCode);
		directCodes->AddOrUpdateCode(utf8Code);
		// Rooms asks for a DIRECT match by code, and a code nobody owns fails as
		// a timeout with nothing said. Print what we are actually searching for,
		// because that is the one fact the failure hides.
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] searching DIRECT for '%s'", utf8Code.c_str());
	}
	else if (search.mode == SlippiMatchmaking::TEAMS)
	{
		std::string utf8Code = SHIFTJISToUTF8(shiftJisCode);
		teamsCodes->AddOrUpdateCode(utf8Code);
	}

	// TODO: Make this work so we dont have to pass shiftJis to mm server
	// search.connectCode = SHIFTJISToUTF8(shiftJisCode).c_str();
	search.connectCode = shiftJisCode;

	// Store this search so we know what was queued for
	lastSearch = search;

	// While we do have another condition that checks characters after being connected, it's nice to give
	// someone an early error before they even queue so that they wont enter the queue and make someone
	// else get force removed from queue and have to requeue
	if (SlippiMatchmaking::IsFixedRulesMode(search.mode))
	{
		// Character check
		if (localSelections.characterId >= 26)
		{
			forcedError = "The character you selected is not allowed in this mode";
			return;
		}

		// Stage check
		if (localSelections.isStageSelected &&
		    std::find(allowedStages.begin(), allowedStages.end(), localSelections.stageId) == allowedStages.end())
		{
			forcedError = "The stage being requested is not allowed in this mode";
			return;
		}
	}
	else if (search.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS)
	{
		auto isMex = SConfig::GetInstance().m_gameType == GAMETYPE_MELEE_MEX;
		// Some special handling for teams since it is being heavily used for unranked
		if (localSelections.characterId >= 26 && !isMex)
		{
			forcedError = "The character you selected is not allowed in this mode";
			return;
		}
	}

#ifndef LOCAL_TESTING
	if (!isEnetInitialized)
	{
		// Initialize enet
		auto res = enet_initialize();
		if (res < 0)
			ERROR_LOG(SLIPPI_ONLINE, "Failed to initialize enet res: %d", res);

		isEnetInitialized = true;
	}

	matchmaking->FindMatch(search);
#endif
}

bool CEXISlippi::doesTagMatchInput(u8 *input, u8 inputLen, std::string tag)
{
	auto jisTag = UTF8ToSHIFTJIS(tag);

	// Check if this tag matches what has been input so far
	bool isMatch = true;
	for (int i = 0; i < inputLen; i++)
	{
		// ERROR_LOG(SLIPPI_ONLINE, "Entered: %X%X. History: %X%X", input[i * 3], input[i * 3 + 1], (u8)jisTag[i * 2],
		//          (u8)jisTag[i * 2 + 1]);
		if (input[i * 3] != (u8)jisTag[i * 2] || input[i * 3 + 1] != (u8)jisTag[i * 2 + 1])
		{
			isMatch = false;
			break;
		}
	}

	return isMatch;
}

void CEXISlippi::handleNameEntryLoad(u8 *payload)
{
	u8 inputLen = payload[24];
	u32 initialIndex = payload[25] << 24 | payload[26] << 16 | payload[27] << 8 | payload[28];
	u8 scrollDirection = payload[29];
	u8 curMode = payload[30];

	auto codeHistory = directCodes.get();
	if (curMode == SlippiMatchmaking::TEAMS)
	{
		codeHistory = teamsCodes.get();
	}

	// Adjust index
	u32 curIndex = initialIndex;
	if (scrollDirection == 1)
	{
		curIndex++;
	}
	else if (scrollDirection == 2)
	{
		curIndex = curIndex > 0 ? curIndex - 1 : curIndex;
	}
	else if (scrollDirection == 3)
	{
		curIndex = 0;
	}

	// Scroll to next tag that
	std::string tagAtIndex = "1";
	while (curIndex >= 0 && curIndex < (u32)codeHistory->length())
	{
		tagAtIndex = codeHistory->get(curIndex);

		// Break if we have found a tag that matches
		if (doesTagMatchInput(payload, inputLen, tagAtIndex))
			break;

		curIndex = scrollDirection == 2 ? curIndex - 1 : curIndex + 1;
	}

	INFO_LOG(SLIPPI_ONLINE, "Idx: %d, InitIdx: %d, Scroll: %d. Len: %d", curIndex, initialIndex, scrollDirection,
	         inputLen);

	tagAtIndex = codeHistory->get(curIndex);
	if (tagAtIndex == "1")
	{
		// If we failed to find a tag at the current index, try the initial index again.
		// If the initial index matches the filter, preserve that suggestion. Without
		// this logic, the suggestion would get cleared
		auto initialTag = codeHistory->get(initialIndex);
		if (doesTagMatchInput(payload, inputLen, initialTag))
		{
			tagAtIndex = initialTag;
			curIndex = initialIndex;
		}
	}

	INFO_LOG(SLIPPI_ONLINE, "Retrieved tag: %s", tagAtIndex.c_str());
	std::string jisCode;
	m_read_queue.clear();

	if (tagAtIndex == "1")
	{
		m_read_queue.push_back(0);
		m_read_queue.insert(m_read_queue.end(), payload, payload + 3 * inputLen);
		m_read_queue.insert(m_read_queue.end(), 3 * (8 - inputLen), 0);
		m_read_queue.push_back(inputLen);
		appendWordToBuffer(&m_read_queue, initialIndex);
		return;
	}

	// Indicate we have a suggestion
	m_read_queue.push_back(1);

	// Convert to tag to shift jis and write to response
	jisCode = UTF8ToSHIFTJIS(tagAtIndex);

	// Write out connect code into buffer, injection null terminator after each letter
	for (int i = 0; i < 8; i++)
	{
		for (int j = i * 2; j < i * 2 + 2; j++)
		{
			m_read_queue.push_back(j < jisCode.length() ? jisCode[j] : 0);
		}

		m_read_queue.push_back(0x0);
	}

	INFO_LOG(SLIPPI_ONLINE, "New Idx: %d. Jis Code length: %d", curIndex, (u8)(jisCode.length() / 2));

	// Write length of tag
	m_read_queue.push_back(jisCode.length() / 2);
	appendWordToBuffer(&m_read_queue, curIndex);
}

void CEXISlippi::prepareOnlineMatchState()
{
	// Rooms: a finished game ends the session HERE, before anything is answered.
	// See s_rooms_end_session - without this Melee restarts the match it was
	// just told about and the room never gets its turn.
	if (s_rooms_end_session && !s_rooms_cleanup_busy.load() &&
	    Common::Timer::GetTimeMs() - s_rooms_end_session_at > 250)
	{
		s_rooms_end_session = false;
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] game over - clearing the match so the room can have it");
		handleConnectionCleanup();
		prepareOnlineMatchState(); // answer again, with nothing to play
		return;
	}

	SConfig::GetInstance().m_EmulationSpeed = 1.0f; // force 100% speed

	// This match block is a VS match with P1 Red Falco vs P2 Red Bowser vs P3 Young Link vs P4 Young Link
	// on Battlefield. The proper values will be overwritten
	static std::vector<u8> onlineMatchBlock = {
	    0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00, 0x1F, 0x00, 0x00,
	    0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
	};

	m_read_queue.clear();

	auto errorState = SlippiMatchmaking::ProcessState::ERROR_ENCOUNTERED;
	SlippiMatchmaking::ProcessState mmState = !forcedError.empty() ? errorState : matchmaking->GetMatchmakeState();

#ifdef LOCAL_TESTING
	if (localSelections.isCharacterSelected || isLocalConnected)
	{
		mmState = SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS;
		isLocalConnected = true;
	}
#endif

	// Watching: there is a match, and everything about it arrived over the watch
	// connections rather than being negotiated. Build what the code below expects
	// to find and tell it we are connected.
	if (isWatching())
	{
		SlippiWatchClient::Picks picks = watch_client->GetPicks();

		if (!slippi_netplay)
		{
			slippi_netplay = std::make_unique<SlippiNetplayClient>(true);
			slippi_netplay->MakeWatcher(SlippiWatchClient::WATCHER_PORT);

			// ⚠️ Nothing from a match this client played EARLIER belongs to the
			// one it is watching. recentMmResult is filled in where matchmaking
			// hands over a netplay client - which a watcher never reaches,
			// because it has just built its own - so whatever was last in there
			// rode along: another match's id, and an items bitfield that
			// prepareOnlineMatchState turns into items ON at high frequency.
			// Items on one screen and not the other is a divergence.
			recentMmResult = SlippiMatchmaking::MatchmakeResult();
			WARN_LOG(SLIPPI_ONLINE, "[Watch] starting %d vs %d on stage %d, seed %08x", picks.character[0],
			         picks.character[1], picks.stage, picks.seed);
		}

		for (u8 i = 0; i < 2; i++)
		{
			SlippiPlayerSelections sel;
			sel.playerIdx = i;
			sel.characterId = picks.character[i];
			sel.characterColor = picks.colour[i];
			sel.isCharacterSelected = true;
			sel.stageId = picks.stage;
			sel.isStageSelected = true;
			// ⚠ The seed the players ran with. Without it the watcher rolls its
			// own and everything random - hazards, item spawns, tumble - happens
			// differently, which is a divergence that looks like a desync.
			sel.rngOffset = picks.seed;
			slippi_netplay->SetRemoteSelections(i, sel);
		}

		// Ours, for a port that is not in the match. It has to look chosen or the
		// gate below never opens, and the block marks port 2 empty so nothing is
		// drawn for it.
		localSelections.playerIdx = SlippiWatchClient::WATCHER_PORT;
		localSelections.isCharacterSelected = true;
		localSelections.isStageSelected = true;
		localSelections.stageId = picks.stage;
		localSelections.rngOffset = picks.seed;

		// ⚠️ And into the netplay client, which is where the code below actually
		// reads them from - localSelections is ours, matchInfo.localPlayerSelections
		// is what prepareOnlineMatchState uses. Leaving the two out of step read as
		// player index 0 and seed 0, which crashed on a null orderedSelections[2]
		// and would have run the match on the wrong seed if it had not.
		slippi_netplay->SetWatchSelections(localSelections);

		localPlayerIndex = SlippiWatchClient::WATCHER_PORT;
		mmState = SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS;
	}

	m_read_queue.push_back(mmState); // Matchmaking State

	u8 localPlayerReady = localSelections.isCharacterSelected;
	u8 remotePlayersReady = 0;

	auto userInfo = user->GetUserInfo();
	u16 alt_stage_mode = 0;

	if (mmState == SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS)
	{
		// ⚠ Not for a watcher. Matchmaking never paired us with anybody, so this
		// answers 0 - which would put the watcher back on a port that IS in the
		// match, and Melee would read a neutral controller for a player who is
		// actually being fed. That is the divergence the old build died of.
		if (!isWatching())
			localPlayerIndex = matchmaking->LocalPlayerIndex();

		if (!slippi_netplay)
		{
#ifdef LOCAL_TESTING
			slippi_netplay = std::make_unique<SlippiNetplayClient>(true);
#else
			slippi_netplay = matchmaking->GetNetplayClient();
#endif

			// This happens on the initial connection to a player. The matchmaking object is ephemeral, it
			// gets re-created when a connection is terminated, that said, it can still be useful to know
			// who we were connected to after they disconnect from us, for example in the case of reporting
			// a match. So let's copy the results.
			recentMmResult = matchmaking->GetMatchmakeResult();

			// Rooms: tell the room where watchers should dial. This is Slippi's
			// own measurement of the socket this match is running on, so there
			// is nothing for us to discover and no STUN to do.
			// ⚠ The second argument is TEST ONLY and is dropped unless peppy.json
			// says lanForTesting. See Rooms::Config::lan_for_testing.
			Rooms::SetAddress(matchmaking->LocalExternalAddress(), matchmaking->LocalLanAddress());

			// Use allowed stages from the matchmaking service and pick a new random stage before sending
			// the selections to the opponent
			allowedStages = recentMmResult.stages;
			if (allowedStages.empty())
			{
				allowedStages = {
				    0x2,  // FoD
				    0x3,  // Pokemon
				    0x8,  // Yoshi's Story
				    0x1C, // Dream Land
				    0x1F, // Battlefield
				    0x20, // Final Destination
				};
			}

			stagePool.clear(); // Clear stage pool so that when we call getRandomStage it will use full list
			localSelections.stageId = getRandomStage();
			slippi_netplay->SetMatchSelections(localSelections);
		}

#ifdef LOCAL_TESTING
		bool isConnected = true;
#else
		auto status = slippi_netplay->GetSlippiConnectStatus();
		bool isConnected = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;

		// If any players are disconnected and the match state is being requested (we are in a lobby),
		// we should just disconnect. This allows for games to finish with a disconnected player but
		// after that the "lobby" is terminated.
		//
		// ⚠ Meaningless for a watcher, and only passes by accident: its stand-in
		// has no peers and matchmaking never paired it, so both sides are zero.
		// Said out loud rather than relied on.
		if (!isWatching() && slippi_netplay->GetActivePlayerIndices().size() != matchmaking->RemotePlayerCount())
		{
			isConnected = false;
		}
#endif

		if (isConnected)
		{
			auto matchInfo = slippi_netplay->GetMatchInfo();
			remotePlayersReady = 1;
#ifndef LOCAL_TESTING
			u8 remotePlayerCount = isWatching() ? 2 : matchmaking->RemotePlayerCount();
			for (int i = 0; i < remotePlayerCount; i++)
			{
				if (!matchInfo->remotePlayerSelections[i].isCharacterSelected)
				{
					remotePlayersReady = 0;
				}
			}

			if (remotePlayerCount == 1)
			{
				auto isDecider = slippi_netplay->IsDecider();
				localPlayerIndex = isDecider ? 0 : 1;
				remotePlayerIndex = isDecider ? 1 : 0;
			}
#endif
		}
		else
		{
#ifndef LOCAL_TESTING
			// If we get here, our opponent likely disconnected. Let's trigger a clean up
			handleConnectionCleanup();
			prepareOnlineMatchState(); // run again with new state
			return;
#endif
		}

		// Here we are connected, check to see if we should init play session
		//
		// ⚠ Not for a watcher. This opens a reporting session with Slippi for a
		// game this client is not playing, and the report that would close it is
		// deliberately never sent.
		if (!isPlaySessionActive && !isWatching())
		{
			slprs_exi_device_start_new_reporter_session(slprs_exi_device_ptr);
			isPlaySessionActive = true;
		}
	}
	else
	{
		slippi_netplay = nullptr;
	}

	u32 rngOffset = 0;
	std::string localPlayerName = "";
	std::string oppName = "";
	std::string p1Name = "";
	std::string p2Name = "";
	s8 p1Rank = 0;
	s8 p2Rank = 0;
	u8 chatMessageId = 0;
	u8 chatMessagePlayerIdx = 0;
	u8 sentChatMessageId = 0;

#ifdef LOCAL_TESTING
	localPlayerIndex = 0;
	sentChatMessageId = localChatMessageId;
	chatMessagePlayerIdx = 0;
	localChatMessageId = 0;
	// in CSS p1 is always current player and p2 is opponent
	localPlayerName = p1Name = userInfo.displayName;
	oppName = p2Name = "Player 2";
	p1Rank = 8;
	p2Rank = 15;
#endif

	SlippiDesyncRecoveryResp desync_recovery;
	if (slippi_netplay)
	{
		desync_recovery = slippi_netplay->GetDesyncRecoveryState();
	}

	// If we have an active desync recovery and haven't received the opponent's state, wait
	if (desync_recovery.is_recovering && desync_recovery.is_waiting)
	{
		remotePlayersReady = 0;
	}

	if (desync_recovery.is_error)
	{
		// If desync recovery failed, just disconnect connection. Hopefully this will almost never happen
		handleConnectionCleanup();
		prepareOnlineMatchState(); // run again with new state
		return;
	}

	m_read_queue.push_back(localPlayerReady);   // Local player ready
	m_read_queue.push_back(remotePlayersReady); // Remote players ready
	m_read_queue.push_back(localPlayerIndex);   // Local player index
	m_read_queue.push_back(remotePlayerIndex);  // Remote player index

	// Set chat message if any
	if (slippi_netplay)
	{
		auto isSingleMode = matchmaking && matchmaking->RemotePlayerCount() == 1;
		bool isChatEnabled = isSlippiChatEnabled();
		sentChatMessageId = slippi_netplay->GetSlippiRemoteSentChatMessage(isChatEnabled);

		// Prevent processing a message in the same frame
		if (sentChatMessageId <= 0)
		{
			auto remoteMessageSelection = slippi_netplay->GetSlippiRemoteChatMessage(isChatEnabled);
			chatMessageId = remoteMessageSelection.messageId;
			chatMessagePlayerIdx = remoteMessageSelection.playerIdx;
			if (chatMessageId == SlippiPremadeText::CHAT_MSG_CHAT_DISABLED && !isSingleMode)
			{
				// Clear remote chat messages if we are on teams and the player has chat disabled.
				// Could also be handled on SlippiNetplay if the instance had acccess to the current connection mode
				chatMessageId = chatMessagePlayerIdx = 0;
			}
		}
		else
		{
			chatMessagePlayerIdx = localPlayerIndex;
		}

		if (isSingleMode || !matchmaking)
		{
			chatMessagePlayerIdx = sentChatMessageId > 0 ? localPlayerIndex : remotePlayerIndex;
		}
		// in CSS p1 is always current player and p2 is opponent
		localPlayerName = p1Name = userInfo.displayName;
	}

	// NOTICE_LOG(SLIPPI_ONLINE, "%d, %d", localPlayerReady, remotePlayersReady);

	// Put our own last character back where the draft will look for it.
	//
	// ⚠️ Only while we are NOT ready - which is the draft, and exactly when this
	// block still holds the previous game's characters. The branch below
	// overwrites it with the real selections the moment both players have
	// chosen, so this cannot leak into an actual match.
	if (have_my_last && !(localPlayerReady && remotePlayersReady) && localPlayerIndex < 4)
	{
		onlineMatchBlock[0x60 + localPlayerIndex * 0x24] = my_last_char;
		onlineMatchBlock[0x63 + localPlayerIndex * 0x24] = my_last_color;
	}

	if (localPlayerReady && remotePlayersReady)
	{
		auto isDecider = slippi_netplay->IsDecider();
		u8 remotePlayerCount = isWatching() ? 2 : matchmaking->RemotePlayerCount();
		auto matchInfo = slippi_netplay->GetMatchInfo();
		SlippiPlayerSelections lps = matchInfo->localPlayerSelections;
		auto rps = matchInfo->remotePlayerSelections;

#ifdef LOCAL_TESTING
		lps.playerIdx = 0;

		// By default Local testing for teams is against
		// 1 RED TEAM Falco
		// 2 BLUE TEAM Falco
		for (int i = 0; i <= SLIPPI_REMOTE_PLAYER_MAX; i++)
		{
			if (i == 0)
			{
				rps[i].characterColor = 1;
				rps[i].teamId = 0;
			}
			else
			{
				rps[i].characterColor = 2;
				rps[i].teamId = 1;
			}

			rps[i].characterId = 0x14;
			rps[i].playerIdx = i + 1;
			rps[i].isCharacterSelected = true;
		}

		remotePlayerCount = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS ? 3 : 1;

		oppName = std::string("Player");
#endif

		// Check if someone is picking dumb characters in non-direct
		auto localCharOk = lps.characterId < 26;
		auto remoteCharOk = true;
		INFO_LOG(SLIPPI_ONLINE, "remotePlayerCount: %d", remotePlayerCount);
		for (int i = 0; i < remotePlayerCount; i++)
		{
			if (rps[i].characterId >= 26)
				remoteCharOk = false;
		}

		// TODO: Ideally remotePlayerSelections would just include everyone including the local player
		// TODO: Would also simplify some logic in the Netplay class

		// Here we are storing pointers to the player selections. That means that we can technically modify
		// the values from here, which is probably not the cleanest thing since they're coming from the netplay class.
		// Unfortunately, I think it might be required for the overwrite stuff to work correctly though, maybe on a
		// tiebreak in ranked?
		std::vector<SlippiPlayerSelections *> orderedSelections(remotePlayerCount + 1);
		orderedSelections[lps.playerIdx] = &lps;
		for (int i = 0; i < remotePlayerCount; i++)
		{
			orderedSelections[rps[i].playerIdx] = &rps[i];
		}

		// Overwrite selections
		for (int i = 0; i < overwrite_selections.size(); i++)
		{
			const auto &ow = overwrite_selections[i];

			orderedSelections[i]->characterId = ow.characterId;
			orderedSelections[i]->characterColor = ow.characterColor;
			orderedSelections[i]->stageId = ow.stageId;
		}

		// Overwrite stage information. Make sure everyone loads the same stage
		u16 stageId = 0x1F; // Default to battlefield if there was no selection
		for (const auto &selections : orderedSelections)
		{
			if (!selections->isStageSelected)
				continue;

			// Stage selected by this player, use that selection
			stageId = selections->stageId;
			alt_stage_mode = selections->alt_stage_mode;
			break;
		}

		if (SlippiMatchmaking::IsFixedRulesMode(lastSearch.mode))
		{
			// If we enter one of these conditions, someone is doing something bad, clear the lobby

			if (!localCharOk)
			{
				handleConnectionCleanup();
				forcedError = "The character you selected is not allowed in this mode";
				prepareOnlineMatchState();
				return;
			}

			if (!remoteCharOk)
			{
				handleConnectionCleanup();
				prepareOnlineMatchState();
				return;
			}

			if (std::find(allowedStages.begin(), allowedStages.end(), stageId) == allowedStages.end())
			{
				handleConnectionCleanup();
				prepareOnlineMatchState();
				return;
			}
		}
		else if (lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS)
		{
			auto isMex = SConfig::GetInstance().m_gameType == GAMETYPE_MELEE_MEX;

			if (!localCharOk && !isMex)
			{
				handleConnectionCleanup();
				forcedError = "The character you selected is not allowed in this mode";
				prepareOnlineMatchState();
				return;
			}

			if (!remoteCharOk && !isMex)
			{
				handleConnectionCleanup();
				prepareOnlineMatchState();
				return;
			}
		}

		// Set rng offset
		rngOffset = isDecider ? lps.rngOffset : rps[0].rngOffset;
		INFO_LOG(SLIPPI_ONLINE, "Rng Offset: 0x%x", rngOffset);

		// Tell the ROOM what we are actually playing, so the band across the top
		// of the room screen can draw it for everybody who is not one of us.
		//
		// ⚠️ From HERE, not from the draft's own steps. A step reports the field
		// it is NOT choosing as 0 - and 0 is Captain Falcon, and 0 is a real
		// stage. Read off the steps, the ban published two Captain Falcons and a
		// later step overwrote Dream Land with stage 0. These are the resolved
		// values, the same ones that go into the match block below, and they are
		// only reached once both players have chosen.
		//
		// ⚠️ Ours only. pd_tick takes a character from the player it belongs to
		// and ignores anyone else's, so each of the two reports itself.
		if (!isWatching())
			Rooms::ReportPick(lps.characterId, lps.characterColor, stageId);

		// Check if everyone is the same color
		auto firstTeamId = orderedSelections[0]->teamId;
		bool areAllSameTeam = true;
		for (const auto &s : orderedSelections)
		{
			// ERROR_LOG(SLIPPI_ONLINE, "[%d] First team: %d. Team: %d. LocalPlayer: %d", s->playerIdx, color,
			// s->teamId, localPlayerIndex);
			if (s->teamId != firstTeamId)
			{
				areAllSameTeam = false;
			}
		}

		// Choose random team assignments
		// Previously there was a bug here where the shuffle was not consistent across platforms given the same seed,
		// this would cause desyncs during cross platform play (different teams). Got around this by no longer using
		// the shuffle function...
		std::vector<std::vector<u8>> teamAssignmentPermutations = {
		    {0, 0, 1, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}, {1, 0, 0, 1}, {0, 1, 0, 1}, {1, 0, 1, 0},
		};
		auto teamAssignments = teamAssignmentPermutations[rngOffset % teamAssignmentPermutations.size()];

		auto isTeams = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS;

		// Overwrite player character choices
		for (auto &s : orderedSelections)
		{
			if (!s->isCharacterSelected)
			{
				continue;
			}

			auto teamId = isTeams ? s->teamId : 0;
			if (isTeams && areAllSameTeam)
			{
				// Overwrite teamId. Color is overwritten by ASM
				teamId = teamAssignments[s->playerIdx];
			}

			// ERROR_LOG(SLIPPI_ONLINE, "idx: %d, char: %d, team: %d", s->playerIdx, s->characterId, teamId);

			// Overwrite player character
			onlineMatchBlock[0x60 + (s->playerIdx) * 0x24] = s->characterId;
			onlineMatchBlock[0x63 + (s->playerIdx) * 0x24] = s->characterColor;
			onlineMatchBlock[0x67 + (s->playerIdx) * 0x24] = 0;
			onlineMatchBlock[0x69 + (s->playerIdx) * 0x24] = teamId;
		}

		// Handle character coloring. This normally wouldn't be necessary but in the case where one person selects Zelda
		// and one person selects Sheik of the same color, the game wont automatically force the color changes
		std::unordered_map<u16, u8> colorCounts;
		for (int i = 0; i < SLIPPI_PLAYER_COUNT_MAX; i++)
		{
			if (onlineMatchBlock[0x61 + i * 0x24] != 0)
				continue;

			// Use onlineMatchBlock for charId and color because it may have just been overwritten by team assignment
			// logic
			u8 charId = onlineMatchBlock[0x60 + i * 0x24];
			u8 color = onlineMatchBlock[0x63 + i * 0x24];
			u8 teamId = onlineMatchBlock[0x69 + i * 0x24];

			// Make key including char id and char color (or teams id if teams)
			charId = charId == 0x13 ? 0x12 : charId; // Force Sheik to count with Zelda
			u16 key = static_cast<u16>(charId) << 8 | static_cast<u16>(isTeams ? teamId : color);

			// Set the shade of the fighter and increment the count
			u8 &count = colorCounts[key];
			onlineMatchBlock[0x67 + (0x24 * i)] = count;
			count += 1;
		}

		// ⚠️ Remember what WE picked, against ourselves rather than against a
		// port. The port is decided fresh every pairing - localPlayerIndex is
		// "isDecider ? 0 : 1" - and the draft's defaults come from this block,
		// which is indexed by port. Confirmed from a log: a player who was port 1
		// with Dr Mario came back as port 0 and was offered the OTHER player's
		// Falco, because Falco was still sitting in port 0's slot.
		my_last_char = lps.characterId;
		my_last_color = lps.characterColor;
		have_my_last = true;

		// Set teams mode
		onlineMatchBlock[0x8] = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS ? 1 : 0;
		// onlineMatchBlock[0x8] = remotePlayerCount >= 2 ? 1 : 0; // TODO: If we dont set it to teams, it crashes
		// sometimes

		// Set p3/p4 player type to human or none depending on the amount of players
		//
		// ⚠️ A watcher has remotePlayerCount 2 - the two people playing - but it is
		// not a third player, it is sitting on port 2 precisely because that port is
		// NOT in the match. Left to the normal rule this marks port 2 human and puts
		// a motionless fighter in the game.
		onlineMatchBlock[0x61 + 2 * 0x24] = (remotePlayerCount >= 2 && !isWatching()) ? 0 : 3;
		onlineMatchBlock[0x61 + 3 * 0x24] = remotePlayerCount >= 3 ? 0 : 3;

		u16 *stage = (u16 *)&onlineMatchBlock[0xE];
		*stage = Common::swap16(stageId);

		// Turn pause off in unranked/ranked, on in other modes
		auto pauseAllowed = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::DIRECT;
		u8 *gameBitField3 = (u8 *)&onlineMatchBlock[2];
		*gameBitField3 = pauseAllowed ? *gameBitField3 & 0xF7 : *gameBitField3 | 0x8;
		//*gameBitField3 = *gameBitField3 | 0x8;

		// Overwrite alt_stage_mode if in ranked
		auto stage_selection_mode = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::DIRECT ||
		                            lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS;
		if (!stage_selection_mode)
		{
			alt_stage_mode = 0;
		}

		// Handle desync recovery. The default values in desync_recovery.state are 480 seconds (8 min timer) and
		// 4-stock/0 percent damage for the fighters. That means if we are not in a desync recovery state, the
		// state of the timer and fighters will be restored to the defaults
		u32 *seconds_remaining = reinterpret_cast<u32 *>(&onlineMatchBlock[0x10]);
		*seconds_remaining = Common::swap32(desync_recovery.state.seconds_remaining);

		for (int i = 0; i < 4; i++)
		{
			onlineMatchBlock[0x62 + i * 0x24] = desync_recovery.state.fighters[i].stocks_remaining;

			u16 *current_health = reinterpret_cast<u16 *>(&onlineMatchBlock[0x70 + i * 0x24]);
			*current_health = Common::swap16(desync_recovery.state.fighters[i].current_health);
		}
	}

	// Configure mode, timer, stocks, etc
	if (lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::PARTY)
	{
		onlineMatchBlock[0x0] = 0x12; // Time mode, 4 char players in UI, count down timer
		onlineMatchBlock[0x3] = 0xCC; // Show score UI
		u32 *match_timer = reinterpret_cast<u32 *>(&onlineMatchBlock[0x10]);
		*match_timer = Common::swap32(5 * 60); // 5 Minute timer
	}
	else
	{
		// Reset everything. I still feel like maybe the onlineMatchBlock should not be static so we don't have to reset
		// states like this but I'm a bit worried about making that change and causing weird bugs.
		onlineMatchBlock[0x0] = 0x32; // Stock mode, 4 char players in UI, count down timer
		onlineMatchBlock[0x3] = 0x4C; // Hide score UI
		u32 *match_timer = reinterpret_cast<u32 *>(&onlineMatchBlock[0x10]);
		*match_timer = Common::swap32(8 * 60); // 8 Minute timer
	}

	// Configure items. Have to reset things when there are no items.
	onlineMatchBlock[0xB] = 0xFF;             // Items off
	u64 new_items_value = 0xF80000000F000000; // Default value (all items off)
	if (recentMmResult.items != 0)
	{
		// Set the items bitfield. There are 31 bits each representing one item that can be enabled
		new_items_value |= static_cast<u64>(recentMmResult.items) << 28;

		// Set item frequency to high
		onlineMatchBlock[0xB] = 3;
	}
	u64 *item_bits = reinterpret_cast<u64 *>(&onlineMatchBlock[0x23]);
	*item_bits = Common::swap64(new_items_value);

	// Add rng offset to output
	appendWordToBuffer(&m_read_queue, rngOffset);

	// Add delay frames to output
	m_read_queue.push_back((u8)SConfig::GetInstance().m_slippiOnlineDelay);

	// Add chat messages id
	m_read_queue.push_back((u8)sentChatMessageId);
	m_read_queue.push_back((u8)chatMessageId);
	m_read_queue.push_back((u8)chatMessagePlayerIdx);

	bool isRanked = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::RANKED;
	if (isRanked)
	{
		// This has to be outside the player ready block because in game setup 2 the players are not
		// ready at the start
		bool showLocalRank = SConfig::GetInstance().bSlippiPlayerRankDisplay;
		bool showOppRank = SConfig::GetInstance().bSlippiOpponentRankDisplay;

		std::array<s8, 2> ranks = {0, 0};
		ranks[localPlayerIndex] = showLocalRank ? matchmaking->GetPlayerRank(localPlayerIndex) : -1;
		ranks[remotePlayerIndex] = showOppRank ? matchmaking->GetPlayerRank(remotePlayerIndex) : -1;

		p1Rank = ranks[0];
		p2Rank = ranks[1];
	}

	m_read_queue.push_back(p1Rank);
	m_read_queue.push_back(p2Rank);

	// Add names to output
	// Always send static local player name
	localPlayerName = ConvertStringForGame(localPlayerName, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), localPlayerName.begin(), localPlayerName.end());

#ifdef LOCAL_TESTING
	std::string defaultNames[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
#endif

	// A watcher was never matchmade, so it has no names to hand out - the tags
	// under the percentages came up blank. The room knows who is playing, and
	// the watch connection knows which of them Slippi made player 0, which is
	// not always the room's host. See SlippiWatchClient::Picks::slot.
	std::string watchNames[2];
	if (isWatching())
	{
		SlippiWatchClient::Picks picks = watch_client->GetPicks();
		Rooms::State rs = Rooms::Latest();
		for (int i = 0; i < 2; i++)
		{
			size_t slot = picks.slot[i];
			if (slot < rs.active.size())
				watchNames[i] = rs.active[slot].name;
		}
	}

	for (int i = 0; i < 4; i++)
	{
		std::string name = isWatching() && i < 2 ? watchNames[i] : matchmaking->GetPlayerName(i);
#ifdef LOCAL_TESTING
		name = defaultNames[i];
#endif
		name = ConvertStringForGame(name, MAX_NAME_LENGTH);
		m_read_queue.insert(m_read_queue.end(), name.begin(), name.end());
	}

	// Create the opponent string using the names of all players on opposing teams
	std::vector<std::string> opponentNames = {};
	int teamIdx = onlineMatchBlock[0x69 + localPlayerIndex * 0x24];
	for (int i = 0; i < 4; i++)
	{
		auto isTeams = lastSearch.mode == SlippiMatchmaking::OnlinePlayMode::TEAMS;
		auto isSameTeam = onlineMatchBlock[0x69 + i * 0x24] == teamIdx;
		auto playerIsHuman = onlineMatchBlock[0x61 + i * 0x24] == 0;
		if (localPlayerIndex == i || !playerIsHuman || (isSameTeam && isTeams))
			continue;

		auto name = isWatching() && i < 2 ? watchNames[i] : matchmaking->GetPlayerName(i);
		if (name != "")
			opponentNames.push_back(name);
	}

	auto numOpponents = opponentNames.size() == 0 ? 1 : opponentNames.size();
	auto charsPerName = (MAX_NAME_LENGTH - (numOpponents - 1)) / numOpponents;
	std::string oppText = "";
	for (auto &name : opponentNames)
	{
		if (oppText != "")
			oppText += "/";

		oppText += TruncateLengthChar(name, charsPerName);
	}

	oppName = ConvertStringForGame(oppText, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), oppName.begin(), oppName.end());

#ifdef LOCAL_TESTING
	std::string defaultConnectCodes[] = {"PLYR#001", "PLYR#002", "PLYR#003", "PLYR#004"};
#endif

	auto playerInfo = matchmaking->GetPlayerInfo();
	for (int i = 0; i < 4; i++)
	{
		std::string connectCode = i < playerInfo.size() ? playerInfo[i].connectCode : "";
#ifdef LOCAL_TESTING
		connectCode = defaultConnectCodes[i];
#endif
		connectCode = ConvertConnectCodeForGame(connectCode);
		m_read_queue.insert(m_read_queue.end(), connectCode.begin(), connectCode.end());
	}

#ifdef LOCAL_TESTING
	std::string defaultUids[] = {"l6dqv4dp38a5ho6z1sue2wx2adlp", "jpvducykgbawuehrjlfbu2qud1nv",
	                             "k0336d0tg3mgcdtaukpkf9jtf2k8", "v8tpb6uj9xil6e33od6mlot4fvdt"};
#endif

	for (int i = 0; i < 4; i++)
	{
		std::string uid = i < playerInfo.size() ? playerInfo[i].uid : ""; // UIDs are 28 characters + 1 null terminator
#ifdef LOCAL_TESTING
		uid = defaultUids[i];
#endif
		uid.resize(29); // ensure a null terminator at the end
		m_read_queue.insert(m_read_queue.end(), uid.begin(), uid.end());
	}

	// Add error message if there is one
	auto errorStr = !forcedError.empty() ? forcedError : matchmaking->GetErrorMessage();
	errorStr = ConvertStringForGame(errorStr, 120);
	m_read_queue.insert(m_read_queue.end(), errorStr.begin(), errorStr.end());

	// Add the match struct block to output
	m_read_queue.insert(m_read_queue.end(), onlineMatchBlock.begin(), onlineMatchBlock.end());

	// Add match id to output
	std::string matchId = recentMmResult.id;
	matchId.resize(51);
	m_read_queue.insert(m_read_queue.end(), matchId.begin(), matchId.end());

	// Add alt stage mode to output
	m_read_queue.push_back(static_cast<u8>(alt_stage_mode));
}

u16 CEXISlippi::getRandomStage()
{
	static u16 selectedStage;

	// Reset stage pool if it's empty
	if (stagePool.empty())
	{
		stagePool.insert(stagePool.end(), allowedStages.begin(), allowedStages.end());
	}

	// Get random stage
	int randIndex = generator() % stagePool.size();
	selectedStage = stagePool[randIndex];

	// Remove last selection from stage pool
	stagePool.erase(stagePool.begin() + randIndex);

	return selectedStage;
}

void CEXISlippi::setMatchSelections(u8 *payload)
{
	SlippiPlayerSelections s;

	s.teamId = payload[0];
	s.characterId = payload[1];
	s.characterColor = payload[2];
	s.isCharacterSelected = payload[3];

	s.stageId = Common::swap16(&payload[4]);
	u8 stageSelectOption = payload[6];
	// u8 onlineMode = payload[7];
	s.alt_stage_mode = payload[8];

	s.isStageSelected = stageSelectOption == 1 || stageSelectOption == 3;
	if (stageSelectOption == 3)
	{
		// If stage requested is random, select a random stage
		s.stageId = getRandomStage();
	}
	INFO_LOG(SLIPPI, "LPS set char: %d, iSS: %d, %d, stage: %d, alt stage: %d, team: %d", s.isCharacterSelected,
	         stageSelectOption, s.isStageSelected, s.stageId, s.alt_stage_mode, s.teamId);

	s.rngOffset = generator() % 0xFFFF;

	// Merge these selections
	localSelections.Merge(s);

	if (slippi_netplay)
	{
		slippi_netplay->SetMatchSelections(localSelections);
	}
}

// Rooms: the menu asked for a room.
//
// Two bytes: which kind, and whether it is listed. Done on a detached thread
// because this talks to the network and the CPU thread is mid-frame. Melee gets
// no answer back through this command - it finds out the room exists by asking
// for the room list, the same way it would find anybody else's.
void CEXISlippi::handleRoomCreate(u8 *payload)
{
	static const char *kModes[] = {"singles", "doubles", "ironmans", "crew", "tournament"};
	const u8 mode = payload[0];
	const bool listed = payload[1] != 0;

	if (mode >= sizeof(kModes) / sizeof(kModes[0]))
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] create asked for mode %d, which does not exist", mode);
		return;
	}

	tellRoomsWhoWeAre();

	std::string name = kModes[mode];

	// Said HERE, synchronously, not from the thread. Making a room is two
	// network calls and the room screen loads while they are still in flight -
	// so without this it asks "are we in a room" before anything can say yes,
	// and draws the public list for a moment before the room it just made.
	Rooms::BeginEnter();

	// Enters the room it just made, so the person who opened it is in it. The
	// heartbeat is what actually creates the membership row - pd_tick inserts on
	// its first call - so there is no separate join to fall out of step with.
	std::thread([name, listed]() {
		std::string room = Rooms::CreateRoom(name, listed);
		if (room.empty())
		{
			// It never got made. Take the claim back rather than leave the
			// screen showing a room that does not exist.
			ERROR_LOG(SLIPPI_ONLINE, "[Rooms] the room was not created");
			Rooms::AbandonEnter();
			return;
		}
		Rooms::Enter(room);
	}).detach();
}
// Rooms: pressed Start, or stepped back out of the queue.
//
// Takes effect on the next tick rather than now. Nothing here waits on the
// network - the heartbeat is already running and will carry it.
void CEXISlippi::handleRoomQueue(u8 *payload)
{
	Rooms::SetQueued(payload[0] != 0);
}

// Rooms: out of the room altogether, from a held B.
//
// ⚠️ payload[0] says whether there was a room to leave at all. Walking off the
// list of PUBLIC rooms is not leaving one - you were never in it - and saying
// otherwise drops the client out of the room it was already sitting in.
//
// ⚠️ Rooms::Leave() blocks, for the current nap plus one round trip. That is
// deliberate and this is the right place to pay it: the member row has to
// actually go before anything else happens, and Melee is on its way out of the
// scene, where a short stall does not show.
void CEXISlippi::handleRoomLeave(u8 *payload)
{
	if (!payload[0])
	{
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] leaving the room list");
		return;
	}

	WARN_LOG(SLIPPI_ONLINE, "[Rooms] leaving the room");
	Rooms::SetQueued(false);
	Rooms::Leave();
}

// Rooms: the owner turning the stage draft on or off.
//
// ⚠ Not checked here. The server decides who may change this - pd_set_stage_draft
// updates nothing unless the caller owns the room - because a client that has
// been told "you are the owner" is still a client, and the room screen only
// hides the prompt rather than enforcing anything.
// Rooms: what the pad should do in the draft this frame.
//
// A room whose stages are random still goes through the draft, because that is
// where characters are chosen. Only its stage half - steps 0 and 1, a ban then a
// pick - has to answer itself, and the draft only ever asks its OWN player for a
// step, so those cannot be skipped from here. They get answered by playing the
// screen: sweep the cursor a random way along, press A, and press A again on
// the OK that comes up. ⚠ BOTH presses. One press only marks the stage - the
// first build banned perfectly and then sat on the OK/Redo panel forever.
//
// ⚠ Started and stopped by MESSAGES, never by a timer:
//   * step 0's actor is the player who bans first, which both clients already
//     work out the same way - see ROOM_STATE_BAN_FIRST.
//   * step 1's actor waits until the opponent's step 0 has actually arrived.
//   * either stops the moment its own step completes.
//
// Two bytes back: whether to drive, and how far along to sweep before pressing.
// The distance is rolled ONCE per step here rather than in the ASM, which has no
// random number to hand and no memory between frames worth trusting.
void CEXISlippi::prepareRoomDraftDrive()
{
	m_read_queue.clear();

	u8 drive = 0;

	// ⚠ A NEW DRAFT WIPES WHAT THE LAST ONE DID. draft_last_local_step is
	// "my step has landed, stop driving", and it was only ever set - so the first
	// draft of a room worked and every draft after it thought its step was
	// already done and drove nothing. Random stages worked once per room.
	//
	// The draft scene is the only thing that asks this question, every frame
	// while it is up, so a GAP in the asking is the draft ending. Half a second
	// is far longer than a frame and far shorter than a match.
	u64 now = Common::Timer::GetTimeMs();
	if (now - draft_drive_last_ask > 500)
	{
		draft_last_local_step = -1;
		draft_drive_armed = false;
		draft_drive_frame = 0;
		draft_drive_listen = 0;
		draft_fetch_step = -1;
		draft_opp_step_done = -1;
	}
	draft_drive_last_ask = now;

	Rooms::State rs = Rooms::Latest();
	bool random_stages = Rooms::InRoom() && !rs.stage_draft;

	if (random_stages && slippi_netplay && !isWatching())
	{
		// ⚠ WHICH STEP IS OURS COMES FROM THE DRAFT. It used to be worked out
		// here, from the same rule the room publishes as ROOM_STATE_BAN_FIRST -
		// and the two disagreed. One client announced "driving draft step 0" and
		// auto-pressed into a screen that was waiting for its OPPONENT to ban,
		// while the draft sat on the other client waiting for a human. Fourteen
		// minutes later somebody banned by hand and nothing lined up again.
		//
		// Two copies of a rule is one too many when only one of them decides
		// anything. The draft asks each client about the step it is NOT
		// performing, every frame, starting immediately - so the step it asks
		// about is the opponent's and the other one is ours. No rule, no race
		// with the room's own state, and nothing to keep in sync.
		//
		// The client performing step 0 hears nothing, because it has nothing to
		// wait for. That silence is the answer for it, after long enough that a
		// first ask would certainly have arrived.
		int my_step = -1;
		if (draft_fetch_step == 0)
			my_step = 1;               // asked about the ban, so the pick is ours
		else if (draft_fetch_step == 1)
			my_step = 0;               // asked about the pick, so the ban was ours
		else if (draft_fetch_step < 0 && draft_drive_listen >= ROOM_DRIVE_LISTEN_FRAMES)
			my_step = 0;               // asked about nothing at all: the ban is ours

		if (draft_drive_listen < ROOM_DRIVE_LISTEN_FRAMES)
			draft_drive_listen++;

		// ⚠ The stage half belongs to the ROOM, not to the players, so both of
		// them are locked out for all of it - the one acting, between its own
		// presses, and the one who is only waiting. It starts on the FIRST frame
		// the draft asks, before anything is known about whose step this is,
		// which is the point: the window a player could still steer in was the
		// time spent working that out.
		//
		// ⚠ It ends when BOTH STAGE STEPS ARE DONE - asked directly, not guessed
		// from how far the counters have got. The first version said
		// `draft_fetch_step < 2 && draft_last_local_step < 2`, and that deadlocked
		// the draft: the client performing steps 0 and 2 never FETCHES step 2, it
		// performs it, so its fetch counter stops at 1 and its completed counter
		// stops at 0. Both stay under two forever, so it stayed locked, so it
		// could never complete step 2 - while the other client unlocked properly
		// and waited for a partner who was frozen. Nobody could pick a character.
		// ⚠ Each side of these is a REMEMBERED fact, not a question asked now.
		// GetGamePrepResults pops everything it passes over, so probing it for a
		// step that is not at the front throws away the opponent's stage pick.
		bool step0_done = (my_step == 0 && draft_last_local_step >= 0) ||
		                  draft_opp_step_done >= 0;
		bool step1_done = (my_step == 1 && draft_last_local_step >= 1) ||
		                  draft_opp_step_done >= 1;
		bool stage_phase = !(step0_done && step1_done);

		// And a backstop: if the draft is asking about a CHARACTER step, the stage
		// half is over whatever the rest of this thinks.
		if (draft_fetch_step >= 2)
			stage_phase = false;

		bool already_done = my_step < 0 || draft_last_local_step >= my_step;
		bool my_turn = false;
		if (!already_done)
		{
			if (my_step == 0)
			{
				my_turn = true; // nothing to wait for
			}
			else
			{
				// Only once the ban has actually come back from the opponent.
				SlippiGamePrepStepResults res;
				my_turn = slippi_netplay->GetGamePrepResults(0, res);
			}
		}

		if (my_turn)
		{
			if (!draft_drive_armed)
			{
				draft_drive_armed = true;
				draft_drive_frame = 0;
				// How far to sweep the cursor before pressing, so it is not the
				// same stage every single match. Rolled ONCE per step, here,
				// because the ASM has no random number to hand.
				draft_drive_hold = (u8)(12 + (generator() % 36));
				WARN_LOG(SLIPPI_ONLINE,
				         "[Rooms] driving draft step %d, sweep %d (draft asked about %d)",
				         my_step, draft_drive_hold, draft_fetch_step);
			}

			// ⚠ The FRAME COUNT lives here, not in the ASM. This is asked once
			// a frame, so counting the asks is counting the frames, and it keeps
			// the game side to "read a byte, set a button" with no memory of its
			// own to get out of step.
			// Four phases, in frames since this step armed: sweep the cursor,
			// press A on whatever it landed on, LET GO while the OK/Redo panel
			// comes up, then press A again on OK - which is already the
			// highlighted button, so there is nothing to move onto first.
			u32 t = draft_drive_frame;
			u32 pick_end = draft_drive_hold + ROOM_DRIVE_PRESS_FRAMES;
			u32 gap_end = pick_end + ROOM_DRIVE_GAP_FRAMES;
			u32 ok_end = gap_end + ROOM_DRIVE_PRESS_FRAMES;

			if (t < draft_drive_hold)
				drive = ROOM_DRIVE_SWEEP;
			else if (t < pick_end)
				drive = ROOM_DRIVE_PRESS; // onto the stage
			else if (t < gap_end)
				// LOCK, not NOTHING: it zeroes our buttons too, so it IS the
				// release the next press needs - while still keeping the player
				// out. NOTHING here would hand the cursor back for a third of a
				// second in the middle of the roulette.
				drive = ROOM_DRIVE_LOCK;
			else if (t < ok_end)
				drive = ROOM_DRIVE_PRESS; // onto OK
			else
				drive = ROOM_DRIVE_LOCK; // pressed; waiting for the step to land

			draft_drive_frame++;
		}
		else if (stage_phase)
		{
			// Not our turn, or not yet worked out whose it is. Either way the
			// player does not get to touch this screen.
			drive = ROOM_DRIVE_LOCK;
		}
	}

	if (!draft_drive_armed)
		draft_drive_frame = 0;

	m_read_queue.push_back(drive);
}

// Should practice end? Asked once a frame while a player is practising in a
// room.
//
// ⚠ A player who goes off to practise is STILL IN THE QUEUE - that is the
// whole point of the offer - so the room will pair them while they are in
// there. Nothing brought them back: the match was arranged, both sides were
// waiting, and one of them was in training with no idea. Reported from the
// first beta night: "when someone was in training mode and a match ended, it
// did not load them into their match and exit them out of training."
//
// Only the ENDING is decided here. RoomTrainSceneDecide already sends a
// finished practice back to the room, and the room already starts a match the
// moment it sees one is ready - so ending the scene is the whole fix.
void CEXISlippi::prepareRoomLeaveTraining()
{
	m_read_queue.clear();

	Rooms::State rs = Rooms::Latest();
	bool leave = Rooms::InRoom() && rs.valid && rs.ready;

	// Said once rather than every frame - this is asked sixty times a second.
	static bool said = false;
	if (leave && !said)
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] a match is ready - leaving practice");
	said = leave;

	m_read_queue.push_back(leave ? 1 : 0);
}

void CEXISlippi::handleRoomStageDraft(u8 *payload)
{
	bool on = payload[0] != 0;
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] asking for the stage draft to be %s",
	         on ? "on" : "off");
	Rooms::SetStageDraft(on);
}

// Rooms: join a room by code.
//
// Starts the heartbeat, which is what actually puts a member row in the room -
// pd_tick inserts on first call. There is no separate join request to get out
// of step with it.
// Whoever Slippi says we are, before anything talks to the room. The connect
// code the room publishes is what the OTHER client will ask Slippi to connect
// to, so it has to be the real one.
void CEXISlippi::tellRoomsWhoWeAre()
{
	SlippiUser::UserInfo info = user->GetUserInfo();
	Rooms::SetIdentity(info.displayName, info.connectCode);
}

// Rooms: watch the match this room is playing.
//
// Both addresses come from the room state we already hold - the two playing
// publish where Slippi's matchmaking server saw their netplay socket, and a
// watcher attaches to that same socket as an extra ENet peer.
//
// ⚠ BOTH of them. Each client sends only its OWN pads and its OWN selections,
// so one connection is half a match and half a match cannot be simulated.
void CEXISlippi::handleRoomWatch()
{
	if (watch_client)
	{
		// ⚠️ A client that gave up has to be let go of, or Y is dead for the rest
		// of the session: the object is still here, so every later press answers
		// "already watching" and the person can never try again.
		SlippiWatchClient::Status st = watch_client->GetStatus();
		if (st == SlippiWatchClient::Status::FAILED || st == SlippiWatchClient::Status::OVER)
		{
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] the last watch ended - starting a new one");
			watch_client.reset();
		}
		else
		{
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] already watching");
			return;
		}
	}

	Rooms::State rs = Rooms::Latest();
	if (rs.watch_targets.size() < 2)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] cannot watch - %d of 2 players have said where they are",
		          (int)rs.watch_targets.size());
		return;
	}

	// Each target is "host:port", and on a TEST RIG a second "host:port" after a
	// space - where that player is on their own network. See
	// Rooms::Config::lan_for_testing.
	auto split = [](const std::string &one, std::string &host, u16 &port) -> bool {
		size_t colon = one.rfind(':');
		if (colon == std::string::npos)
			return false;
		host = one.substr(0, colon);
		port = (u16)atoi(one.substr(colon + 1).c_str());
		return port != 0;
	};

	std::vector<std::string> addrs;
	std::vector<u16> ports;
	std::vector<std::pair<std::string, u16>> fallback;
	for (const auto &target : rs.watch_targets)
	{
		std::string real = target, test;
		size_t space = target.find(' ');
		if (space != std::string::npos)
		{
			real = target.substr(0, space);
			test = target.substr(space + 1);
		}

		std::string host;
		u16 port = 0;
		if (!split(real, host, port))
		{
			ERROR_LOG(SLIPPI_ONLINE, "[Rooms] '%s' is not host:port", real.c_str());
			return;
		}
		addrs.push_back(host);
		ports.push_back(port);

		// ⚠⚠ TEST RIGS ONLY - DELETE BEFORE THE FIRST BETA ⚠⚠
		if (!test.empty() && split(test, host, port))
			fallback.emplace_back(host, port);
	}

	// Port 0: the OS picks one, and STUN then reports whatever the world sees -
	// so it does not need to be predictable, only discoverable.
	//
	// ⚠ The old reasoning here was that a watcher dials out, so its own NAT
	// opens on the way and the players learn where it is from the connection
	// itself. That is true of OUR router and useless: it is the PLAYERS' routers
	// that drop a first packet from a stranger, and they do. Watching worked on a
	// LAN and failed over the internet every time until the punch list was wired
	// up at both ends.
	watch_client = std::make_unique<SlippiWatchClient>(addrs, ports, 0, fallback);

	// ⚠ The watch client publishes its own address, from its own thread, once
	// STUN has answered - see ThreadFunc. Doing it here would mean blocking the
	// game thread on a network round trip the moment somebody presses Y.
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] watching %s and %s", rs.watch_targets[0].c_str(),
	         rs.watch_targets[1].c_str());
}

void CEXISlippi::handleRoomJoin(u8 *payload)
{
	tellRoomsWhoWeAre();
	std::string code((char *)payload, ROOM_CODE_LEN);
	// Melee pads with spaces rather than nulls.
	while (!code.empty() && (code.back() == ' ' || code.back() == '\0'))
		code.pop_back();

	if (code.empty())
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] join was asked for an empty code");
		return;
	}
	Rooms::Enter(code);
}

// Rooms: everything the room screen draws, in one fixed-size reply.
//
// ⚠️ The layout is written out in EXI_DeviceSlippi.h and duplicated by hand in
// the game module's rooms.h. Change both or the module reads the wrong bytes -
// it will not fail to build, it will just draw nonsense.
//
// Never blocks. The heartbeat already fetched this; here we only copy out what
// it last saw, so asking every frame costs nothing.
// Rooms: go and fetch the public rooms.
//
// One byte, the mode to filter by, or 0xFF for every kind - which is what
// Public asks for, since it is reached before a kind has been chosen. Detached,
// because this talks to the network and the CPU thread is mid-frame; the game
// reads the answer later with CMD_ROOM_LIST_READ.
void CEXISlippi::handleRoomList(u8 *payload)
{
	static const char *kModes[] = {"singles", "doubles", "ironmans", "crew", "tournament"};
	const u8 mode = payload[0];

	std::string filter;
	if (mode < sizeof(kModes) / sizeof(kModes[0]))
		filter = kModes[mode];

	std::thread([filter]() { Rooms::FetchRooms(filter); }).detach();
}

// Rooms: what the fetch found.
//
// ⚠️ The layout is in EXI_DeviceSlippi.h and duplicated by hand in the module's
// rooms.h. Change both or the browser draws nonsense without failing to build.
//
// The FETCHED flag matters: "nothing has come back yet" and "there are no
// public rooms" look identical as an empty list, and the browser has to say
// something different for each. A browser that says "no rooms" before its first
// reply has arrived is simply lying.
void CEXISlippi::prepareRoomList()
{
	m_read_queue.clear();

	std::vector<Rooms::Listing> rooms = Rooms::Rooms();
	const size_t n = std::min<size_t>(rooms.size(), ROOM_LIST_MAX);

	m_read_queue.push_back(Rooms::RoomsFetched() ? ROOM_LIST_FETCHED : 0);
	m_read_queue.push_back((u8)n);
	m_read_queue.push_back(0);
	m_read_queue.push_back(0);

	static const char *kModes[] = {"singles", "doubles", "ironmans", "crew", "tournament"};

	for (size_t i = 0; i < ROOM_LIST_MAX; i++)
	{
		u8 entry[ROOM_LIST_STRIDE] = {};

		if (i < n)
		{
			const Rooms::Listing &l = rooms[i];

			// Four characters and a null. Truncated rather than wrapped: a room
			// code is always four, and anything else is a row nobody can type.
			for (size_t c = 0; c < 7 && c < l.code.size(); c++)
				entry[c] = (u8)l.code[c];

			entry[0x08] = ROOM_MODE_UNKNOWN;
			for (size_t m = 0; m < sizeof(kModes) / sizeof(kModes[0]); m++)
			{
				if (l.mode == kModes[m])
				{
					entry[0x08] = (u8)m;
					break;
				}
			}
			entry[0x09] = (u8)std::min(l.players, 255);
			entry[0x0A] = (u8)std::min(l.capacity, 255);

			std::string owner = ConvertStringForGame(l.owner, MAX_NAME_LENGTH);
			owner.resize(32, '\0');
			for (int c = 0; c < 32; c++)
				entry[0x0C + c] = (u8)owner[c];
		}

		for (int b = 0; b < ROOM_LIST_STRIDE; b++)
			m_read_queue.push_back(entry[b]);
	}
}

void CEXISlippi::prepareRoomState()
{
	m_read_queue.clear();

	Rooms::State s = Rooms::Latest();

	u8 flags = 0;
	if (s.valid)
		flags |= 0x01;
	// Which of its two screens the room scene is. Answered from whether we have
	// a room rather than from whether a tick has answered - see ROOM_FLAG_INROOM.
	if (Rooms::InRoom())
		flags |= ROOM_FLAG_INROOM;
	if (s.draft.playing)
		flags |= 0x02;
	// ⚠ Not while the last match is still being put down. The old matchmaking
	// and netplay clients are destroyed on a detached thread and hold their port
	// until they are gone; telling the room it may start now would have the next
	// search racing the last one's cleanup for it.
	if (s.ready && !RoomsCleanupBusy())
		flags |= ROOM_FLAG_READY;
	if (isWatching())
		flags |= ROOM_FLAG_WATCHING;

	// ⚠️ And only when it is OUR match. A connection being up does not say
	// whose: a third person walking into the queue while two others are being
	// introduced has one too, and used to be sent to the draft to counterpick
	// for a game they were not in.
	if (s.ready && matchmaking &&
	    matchmaking->GetMatchmakeState() == SlippiMatchmaking::CONNECTION_SUCCESS)
		flags |= ROOM_FLAG_CONNECTED;

	// Unlisted, and so it has a passcode. The screen stars both out until
	// somebody asks to see them.
	if (!s.listed)
		flags |= ROOM_FLAG_PRIVATE;

	// In the queue. ⚠️ Sent as a flag because the POSITION field cannot say
	// it: pd_tick computes it as "how many are ahead of me, plus one", which
	// is 1 for somebody who has pressed nothing, so 1 means both "first in the
	// queue" and "not in it". The room screen was telling people in the lobby
	// they were in the queue.
	if (Rooms::Queued())
		flags |= ROOM_FLAG_QUEUED;

	auto pick = [](int v) -> u8 {
		return v == Rooms::Draft::NOT_PICKED ? (u8)ROOM_NOT_PICKED : (u8)v;
	};

	m_read_queue.push_back(flags);
	m_read_queue.push_back((u8)std::min<size_t>(s.queue.size(), ROOM_STATE_MAX_QUEUE));
	m_read_queue.push_back((u8)std::min<size_t>(s.lobby.size(), ROOM_STATE_MAX_LOBBY));
	m_read_queue.push_back((u8)std::min(s.position, 255));
	m_read_queue.push_back(pick(s.draft.host_char));
	m_read_queue.push_back((u8)s.draft.host_color);
	m_read_queue.push_back(pick(s.draft.guest_char));
	m_read_queue.push_back((u8)s.draft.guest_color);
	m_read_queue.push_back(pick(s.draft.stage));

	// ROOM_STATE_BAN_FIRST. ⚠️ The PORT, not a yes/no, because the draft is
	// indexed by port and the port is decided fresh every pairing - both sides
	// work this out for themselves and arrive at the same answer, because
	// exactly one of them is the pairing's host.
	u8 ban_first = 0;
	if (slippi_netplay)
	{
		u8 local_port = slippi_netplay->IsDecider() ? 0 : 1;
		ban_first = s.is_host ? local_port : (u8)(1 - local_port);
	}
	m_read_queue.push_back(ban_first);

	// ROOM_STATE_SETTINGS
	u8 settings = 0;
	if (s.stage_draft)
		settings |= ROOM_SETTING_DRAFT;
	if (s.is_owner)
		settings |= ROOM_SETTING_OWNER;
	m_read_queue.push_back(settings);

	// ROOM_STATE_MM. This byte used to be padding, and the room was blind
	// without it: CMD_FIND_OPPONENT goes out, and the only thing that ever
	// comes back is ROOM_FLAG_CONNECTED on success. A search that errors says
	// nothing at all, so the screen waits on a pairing that is already dead.
	u8 mm = 0;
	if (matchmaking)
	{
		auto mm_state = matchmaking->GetMatchmakeState();
		if (mm_state == SlippiMatchmaking::ERROR_ENCOUNTERED)
			mm |= ROOM_MM_FAILED;
		else if (mm_state == SlippiMatchmaking::INITIALIZING ||
		         mm_state == SlippiMatchmaking::MATCHMAKING ||
		         mm_state == SlippiMatchmaking::OPPONENT_CONNECTING)
			mm |= ROOM_MM_SEARCHING;
	}
	m_read_queue.push_back(mm);

	// Names, in a fixed order so the module can index rather than parse: the
	// two playing, then the queue, then the lobby. Missing entries are blank
	// rather than absent, which is what keeps every slot at a known offset.
	auto put_name = [&](const std::string &name) {
		std::string game = ConvertStringForGame(name, MAX_NAME_LENGTH);
		game.resize(ROOM_STATE_NAME_LEN, '\0');
		for (int i = 0; i < ROOM_STATE_NAME_LEN; i++)
			m_read_queue.push_back((u8)game[i]);
	};

	for (int i = 0; i < 2; i++)
		put_name(i < (int)s.active.size() ? s.active[i].name : "");
	for (int i = 0; i < ROOM_STATE_MAX_QUEUE; i++)
		put_name(i < (int)s.queue.size() ? s.queue[i].name : "");
	for (int i = 0; i < ROOM_STATE_MAX_LOBBY; i++)
		put_name(i < (int)s.lobby.size() ? s.lobby[i].name : "");

	// Converted HERE rather than in the game. startFindMatch takes the code as
	// Shift-JIS and hands it to the matchmaking server that way - there is a
	// TODO in Slippi's own code about it - so converting on this side means the
	// module copies bytes and does not have to know about encodings at all.
	//
	// ⚠ ConvertConnectCodeForGame, NOT UTF8ToSHIFTJIS. The separator Slippi
	// puts on the wire is the FULL-WIDTH hash, 0x81 0x94, because that is what
	// Melee's own connect-code entry produces and nothing converts it back on
	// the way to the matchmaking server. An ASCII '#' survives UTF8ToSHIFTJIS
	// untouched and makes a code that is nearly right, which the server treats
	// as a different code entirely - the ticket is accepted and the assignment
	// never comes, so it reads as a timeout rather than a bad code.
	std::string opp = ConvertConnectCodeForGame(s.opponent_code);
	opp.resize(ROOM_STATE_OPPCODE_LEN);  // value-initialised: zero padded
	for (int i = 0; i < ROOM_STATE_OPPCODE_LEN; i++)
		m_read_queue.push_back((u8)opp[i]);

	// Crowns, in the SAME order as the names above, so the module reads the two
	// with one index. Capped at 255 - past that it is not a number anybody is
	// going to read off a band.
	auto put_crowns = [&](const std::vector<Rooms::Player> &v, int slots) {
		for (int i = 0; i < slots; i++)
			m_read_queue.push_back(i < (int)v.size() ? (u8)std::min(v[i].crowns, 255) : (u8)0);
	};
	put_crowns(s.active, 2);
	put_crowns(s.queue, ROOM_STATE_MAX_QUEUE);
	put_crowns(s.lobby, ROOM_STATE_MAX_LOBBY);

	// The room's own code, and the passcode of a private one. Fixed width and
	// blank padded; the passcode is empty for a public room, which has none.
	auto put_fixed = [&](const std::string &v, int len) {
		std::string t = v;
		t.resize(len, '\0');
		for (int i = 0; i < len; i++)
			m_read_queue.push_back((u8)t[i]);
	};
	put_fixed(s.room, ROOM_STATE_CODE_LEN);
	put_fixed(s.passcode, ROOM_STATE_PASS_LEN);
}


void CEXISlippi::prepareFileLength(u8 *payload)
{
	m_read_queue.clear();

	std::string fileName((char *)&payload[0]);

	std::string contents;
	u32 size = gameFileLoader->LoadFile(fileName, contents);

	INFO_LOG(SLIPPI, "Getting file size for: %s -> %d", fileName.c_str(), size);

	// Write size to output
	appendWordToBuffer(&m_read_queue, size);
}

void CEXISlippi::prepareFileLoad(u8 *payload)
{
	m_read_queue.clear();

	std::string fileName((char *)&payload[0]);

	std::string contents;
	u32 size = gameFileLoader->LoadFile(fileName, contents);
	std::vector<u8> buf(contents.begin(), contents.end());

	INFO_LOG(SLIPPI, "Writing file contents: %s -> %d", fileName.c_str(), size);

	// Write the contents to output
	m_read_queue.insert(m_read_queue.end(), buf.begin(), buf.end());
}

void CEXISlippi::prepareGctLength()
{
	m_read_queue.clear();

	u32 size = Gecko::GetGctLength();

	INFO_LOG(SLIPPI, "Getting gct size: %d", size);

	// Write size to output
	appendWordToBuffer(&m_read_queue, size);
}

void CEXISlippi::prepareGctLoad(u8 *payload)
{
	m_read_queue.clear();

	auto gct = Gecko::GenerateGct();

	// This is the address where the codes will be written to
	auto address = Common::swap32(&payload[0]);

	// for (size_t i = 0, e = gct.size(); i < e; ++i)
	//	PowerPC::HostWrite_U8(gct[i], (u32)(address + i));

	// Overwrite the instructions which load address pointing to codeset
	PowerPC::HostWrite_U32(0x3DE00000 | (address >> 16), 0x80001f58); // lis r15, 0xXXXX # top half of address
	PowerPC::HostWrite_U32(0x61EF0000 | (address & 0xFFFF),
	                       0x80001f5C);              // ori r15, r15, 0xXXXX # bottom half of address
	PowerPC::ppcState.iCache.Invalidate(0x80001f58); // This should invalidate both instructions

	// Invalidate the codes
	// for (unsigned int k = address; k < address + gct.size(); k += 32)
	//	PowerPC::ppcState.iCache.Invalidate(k);

	INFO_LOG(SLIPPI, "Preparing to write gecko codes at: 0x%X. %X, %X", address, 0x3DE00000 | (address >> 16),
	         0x61EF0000 | (address & 0xFFFF));

	// PatchEngine::ApplyFramePatches();

	m_read_queue.insert(m_read_queue.end(), gct.begin(), gct.end());
}

std::vector<u8> CEXISlippi::loadPremadeText(u8 *payload)
{
	u8 textId = payload[0];
	std::vector<u8> premadeTextData;
	auto spt = SlippiPremadeText();

	// WARN_LOG(SLIPPI, "SLIPPI premade text texture id: 0x%x", payload[0]);

	if (textId >= SlippiPremadeText::SPT_CHAT_P1 && textId <= SlippiPremadeText::SPT_CHAT_P4)
	{
		auto port = textId - 1;
		std::string playerName;
		if (matchmaking)
			playerName = matchmaking->GetPlayerName(port);
#ifdef LOCAL_TESTING
		std::string defaultNames[] = {"Player 1", "lol u lost 2 dk", "Player 3", "Player 4"};
		playerName = defaultNames[port];
#endif

		// WARN_LOG(SLIPPI, "SLIPPI premade text param: 0x%x", payload[1]);
		u8 paramId = payload[1];

		for (auto it = spt.unsupportedStringMap.begin(); it != spt.unsupportedStringMap.end(); it++)
		{
			playerName = ReplaceAll(playerName.c_str(), it->second, "");        // Remove unsupported chars
			playerName = ReplaceAll(playerName.c_str(), it->first, it->second); // Remap delimiters for premade text
		}

		// Replaces spaces with premade text space
		playerName = ReplaceAll(playerName.c_str(), " ", "<S>");

		if (paramId == SlippiPremadeText::CHAT_MSG_CHAT_DISABLED)
		{
			return premadeTextData = spt.GetPremadeTextData(SlippiPremadeText::SPT_CHAT_DISABLED, playerName.c_str());
		}

		auto chatMessage = spt.premadeTextsParams[paramId];
		std::string param = ReplaceAll(chatMessage.c_str(), " ", "<S>");
		premadeTextData = spt.GetPremadeTextData(textId, playerName.c_str(), param.c_str());
	}
	else
	{
		premadeTextData = spt.GetPremadeTextData(textId);
	}

	// ERROR_LOG(SLIPPI, "SLIPPI premade text (%d):", premadeTextData.size());
	// for (int i = 0; i < premadeTextData.size(); i++)
	//{
	//	WARN_LOG(SLIPPI, "%X", premadeTextData[i]);
	//}

	return premadeTextData;
}

void CEXISlippi::preparePremadeTextLength(u8 *payload)
{
	std::vector<u8> premadeTextData = loadPremadeText(payload);

	m_read_queue.clear();
	// Write size to output
	appendWordToBuffer(&m_read_queue, premadeTextData.size());
}

void CEXISlippi::preparePremadeTextLoad(u8 *payload)
{
	std::vector<u8> premadeTextData = loadPremadeText(payload);

	m_read_queue.clear();
	// Write data to output
	m_read_queue.insert(m_read_queue.end(), premadeTextData.begin(), premadeTextData.end());
}

bool CEXISlippi::isSlippiChatEnabled()
{
	auto chatEnabledChoice = SConfig::GetInstance().m_slippiEnableQuickChat;
	bool res = true;
	switch (lastSearch.mode)
	{
	case SlippiMatchmaking::DIRECT:
		res = chatEnabledChoice == SLIPPI_CHAT_ON || chatEnabledChoice == SLIPPI_CHAT_DIRECT_ONLY;
		break;
	default:
		res = chatEnabledChoice == SLIPPI_CHAT_ON;
		break;
	}
	return res; // default is enabled
}

void CEXISlippi::handleChatMessage(u8 *payload)
{
	if (!isSlippiChatEnabled())
		return;

	int messageId = payload[0];
	INFO_LOG(SLIPPI, "SLIPPI CHAT INPUT: 0x%x", messageId);

#ifdef LOCAL_TESTING
	localChatMessageId = messageId;
#endif

	if (slippi_netplay)
	{
		auto packet = std::make_unique<sf::Packet>();
		//		OSD::AddMessage("[Me]: "+ msg, OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
		slippi_netplay->remoteSentChatMessageId = messageId;
		// use LocalPlayerPort since it actually uses playerIdx which is what we want
		slippi_netplay->WriteChatMessageToPacket(*packet, messageId, slippi_netplay->LocalPlayerPort());
		slippi_netplay->SendAsync(std::move(packet));
	}
}

void CEXISlippi::logMessageFromGame(u8 *payload)
{
	if (payload[0] == 0)
	{
		// The first byte indicates whether to log the time or not
		GENERIC_LOG(LogTypes::SLIPPI, (LogTypes::LOG_LEVELS)payload[1], "%s", (char *)&payload[2]);
	}
	else
	{
		GENERIC_LOG(LogTypes::SLIPPI, (LogTypes::LOG_LEVELS)payload[1], "%s: %llu", (char *)&payload[2],
		            Common::Timer::GetTimeUs());
	}
}

void CEXISlippi::handleLogInRequest()
{
	bool logInRes = user->AttemptLogin();
	if (!logInRes)
	{
		main_frame->LowerRenderWindow();
		user->OpenLogInPage();
		user->ListenForLogIn();
	}
}

void CEXISlippi::handleLogOutRequest()
{
	user->LogOut();
}

void CEXISlippi::handleUpdateAppRequest()
{
	bool isUpdating = user->UpdateApp();
#ifdef _WIN32
	if (isUpdating)
	{
		main_frame->LowerRenderWindow();
		main_frame->DoExit();
	}
#endif
}

void CEXISlippi::prepareOnlineStatus()
{
	m_read_queue.clear();

	auto isLoggedIn = user->IsLoggedIn();
	auto userInfo = user->GetUserInfo();

	u8 appState = 0;
	if (isLoggedIn)
	{
		// Check if we have the latest version, and if not, indicate we need to update
		version::Semver200_version latestVersion(userInfo.latestVersion);
		version::Semver200_version currentVersion(scm_slippi_semver_str);

		appState = latestVersion > currentVersion ? 2 : 1;
	}

	m_read_queue.push_back(appState);

	// Write player name (31 bytes)
	std::string playerName = ConvertStringForGame(userInfo.displayName, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), playerName.begin(), playerName.end());

	// Write connect code (10 bytes)
	std::string connectCode = ConvertConnectCodeForGame(userInfo.connectCode);
	m_read_queue.insert(m_read_queue.end(), connectCode.begin(), connectCode.end());
}

void doConnectionCleanup(std::unique_ptr<SlippiMatchmaking> mm, std::unique_ptr<SlippiNetplayClient> nc)
{
	if (mm)
		mm.reset();

	if (nc)
		nc.reset();

	s_rooms_cleanup_busy.store(false);
}

void CEXISlippi::handleConnectionCleanup()
{
	ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup started...");

	// Handle destructors in a separate thread to not block the main thread
	s_rooms_cleanup_busy.store(true);
	std::thread cleanup(doConnectionCleanup, std::move(matchmaking), std::move(slippi_netplay));
	cleanup.detach();

	// Reset matchmaking
	matchmaking = std::make_unique<SlippiMatchmaking>(slprs_exi_device_ptr, user.get());

	// Disconnect netplay client
	slippi_netplay = nullptr;

	// Clear character selections
	localSelections.Reset();

	// Reset random stage pool
	stagePool.clear();

	// Reset any forced errors
	forcedError.clear();

	// Reset any selection overwrites
	overwrite_selections.clear();

	// A watch belongs to the match that is being cleaned up.
	if (watch_client)
	{
		setCatchUpSpeed(false);
		watch_client.reset();
	}

	// Reset play session
	isPlaySessionActive = false;

#ifdef LOCAL_TESTING
	isLocalConnected = false;
#endif

	ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup completed...");
}

void CEXISlippi::prepareNewSeed()
{
	m_read_queue.clear();

	u32 newSeed = generator() % 0xFFFFFFFF;

	appendWordToBuffer(&m_read_queue, newSeed);
}

void CEXISlippi::handleReportGame(const SlippiExiTypes::ReportGameQuery &query)
{
	std::string matchId = recentMmResult.id;
	SlippiMatchmakingOnlinePlayMode onlineMode = static_cast<SlippiMatchmakingOnlinePlayMode>(query.onlineMode);
	u32 durationFrames = query.frameLength;
	u32 gameIndex = query.gameIndex;
	u32 tiebreakIndex = query.tiebreakIndex;
	s8 winnerIdx = query.winnerIdx;
	int stageId = Common::FromBigEndian(*(u16 *)&query.gameInfoBlock[0xE]);
	u8 gameEndMethod = query.gameEndMethod;
	s8 lrasInitiator = query.lrasInitiator;

	ERROR_LOG(SLIPPI_ONLINE,
	          "Mode: %d / %d, Frames: %d, GameIdx: %d, TiebreakIdx: %d, WinnerIdx: %d, StageId: %d, GameEndMethod: %d, "
	          "LRASInitiator: %d",
	          onlineMode, query.onlineMode, durationFrames, gameIndex, tiebreakIndex, winnerIdx, stageId, gameEndMethod,
	          lrasInitiator);

	// Rooms: the room needs to know who won, so the winner can stay and the
	// loser can go to the back of the queue. Melee just handed us the winning
	// port, so there is nothing to infer.
	//
	// Then the session ENDS. A finished game sends everybody back to the room
	// and the next two are paired from there - rematching in place would skip
	// four of the five steps the room exists for, and somebody watching from the
	// queue would sit through game after game without ever coming up.
	//
	// ⚠ NOT straight back into matchmaking. The old branch requeued here and
	// it is what stopped anyone ever reaching the room: the search restarts
	// inside the few hundred milliseconds the character select is up, Melee
	// starts a match from a result it still had lying around, and the handler
	// that would have sent you to the room never gets a turn. The clearing is
	// deferred instead - see s_rooms_end_session.
	// ⚠ NOT a watcher. Melee ends a watched game the same way it ends a played
	// one, so without this a watcher reports the result of a match it was not in
	// - closing somebody else's pairing, moving a loser who did not lose, and
	// reshuffling the queue on behalf of two people who are still playing.
	//
	// It tears its watch down instead, which is its whole end-of-match.
	if (isWatching())
	{
		WARN_LOG(SLIPPI_ONLINE, "[Watch] the match ended - closing the watch");
		setCatchUpSpeed(false);
		watch_client.reset();
		s_rooms_end_session = true;
		s_rooms_end_session_at = Common::Timer::GetTimeMs();
		return;
	}

	Rooms::State rs = Rooms::Latest();
	if (matchmaking && !rs.match_id.empty())
	{
		int myIdx = matchmaking->LocalPlayerIndex();

		if (winnerIdx >= 0 && winnerIdx < 4)
		{
			Rooms::ReportResult(rs.match_id, winnerIdx == myIdx,
			                    query.players[winnerIdx].stocksRemaining);
		}
		else if (gameEndMethod == 7 && lrasInitiator >= 0)
		{
			// Somebody quit out. Melee names no winner, but it does name who
			// left, and whoever left lost.
			Rooms::ReportResult(rs.match_id, lrasInitiator != myIdx, 0);
		}
		else
		{
			// ⚠ A genuine draw. Nothing is reported, so the pairing stays open
			// rather than recording a winner we would be inventing.
			ERROR_LOG(SLIPPI_ONLINE, "[Rooms] no result to report: winnerIdx %d, myIdx %d, endMethod %d",
			          winnerIdx, myIdx, gameEndMethod);
		}

		if (slippi_netplay)
		{
			WARN_LOG(SLIPPI_ONLINE, "[Rooms] game over - everybody back to the room");
			slippi_netplay->ForceDisconnect();
		}

		s_rooms_end_session = true;
		s_rooms_end_session_at = Common::Timer::GetTimeMs();
	}

	auto userInfo = user->GetUserInfo();

	// We pass `uid` and `playKey` here until the User side of things is
	// ported to Rust.
	uintptr_t gameReport = slprs_game_report_create(userInfo.uid.c_str(), userInfo.playKey.c_str(), onlineMode,
	                                                matchId.c_str(), durationFrames, gameIndex, tiebreakIndex,
	                                                winnerIdx, gameEndMethod, lrasInitiator, stageId);

	auto mmPlayers = recentMmResult.players;

	for (auto i = 0; i < 4; ++i)
	{
		std::string uid = mmPlayers.size() > i ? mmPlayers[i].uid : "";
		u8 slotType = query.players[i].slotType;
		u8 stocksRemaining = query.players[i].stocksRemaining;
		float damageDone = query.players[i].damageDone;
		u8 charId = query.gameInfoBlock[0x60 + 0x24 * i];
		u8 colorId = query.gameInfoBlock[0x63 + 0x24 * i];
		int startingStocks = query.gameInfoBlock[0x62 + 0x24 * i];
		int startingPercent = Common::FromBigEndian(*(u16 *)&query.gameInfoBlock[0x70 + 0x24 * i]);

		ERROR_LOG(SLIPPI_ONLINE,
		          "UID: %s, Port Type: %d, Stocks: %d, DamageDone: %f, CharId: %d, ColorId: %d, StartStocks: %d, "
		          "StartPercent: %d",
		          uid.c_str(), slotType, stocksRemaining, damageDone, charId, colorId, startingStocks, startingPercent);

		uintptr_t playerReport = slprs_player_report_create(uid.c_str(), slotType, damageDone, stocksRemaining, charId,
		                                                    colorId, startingStocks, startingPercent);

		slprs_game_report_add_player_report(gameReport, playerReport);
	}

	// If ranked mode and the game ended with a quit out, this is either a desync or an interrupted game,
	// attempt to send synced values to opponents in order to restart the match where it was left off
	if (onlineMode == SlippiMatchmaking::OnlinePlayMode::RANKED && gameEndMethod == 7)
	{
		SlippiSyncedGameState s;
		s.match_id = matchId;
		s.game_index = gameIndex;
		s.tiebreak_index = tiebreakIndex;
		s.seconds_remaining = query.syncedTimer;
		for (int i = 0; i < 4; i++)
		{
			s.fighters[i].stocks_remaining = query.players[i].syncedStocksRemaining;
			s.fighters[i].current_health = query.players[i].syncedCurrentHealth;
		}

		if (slippi_netplay)
			slippi_netplay->SendSyncedGameState(s);
	}

#ifndef LOCAL_TESTING
	slprs_exi_device_log_game_report(slprs_exi_device_ptr, gameReport);
#endif
}

void CEXISlippi::prepareDelayResponse()
{
	m_read_queue.clear();
	m_read_queue.push_back(1); // Indicate this is a real response

	if (NetPlay::IsNetPlayRunning())
	{
		// If we are using the old netplay, we don't want to add any additional delay, so return 0
		m_read_queue.push_back(0);
	}
	else
	{
		m_read_queue.push_back((u8)SConfig::GetInstance().m_slippiOnlineDelay);
	}
}

void CEXISlippi::handleOverwriteSelections(const SlippiExiTypes::OverwriteSelectionsQuery &query)
{
	overwrite_selections.clear();

	for (int i = 0; i < 4; i++)
	{
		// TODO: I'm pretty sure this contine would cause bugs if we tried to overwrite only player 1
		// TODO: and not player 0. Right now though GamePrep always overwrites both p0 and p1 so it's fine
		// TODO: The bug would likely happen in the prepareOnlineMatchState, it would overwrite the
		// TODO: wrong players I think
		if (!query.chars[i].is_set)
			continue;

		SlippiPlayerSelections s;
		s.isCharacterSelected = true;
		s.characterId = query.chars[i].char_id;
		s.characterColor = query.chars[i].char_color_id;
		s.isStageSelected = true;
		s.stageId = query.stage_id;
		s.playerIdx = i;

		overwrite_selections.push_back(s);
	}
}

void CEXISlippi::handleGamePrepStepComplete(const SlippiExiTypes::GpCompleteStepQuery &query)
{
	SlippiGamePrepStepResults res;
	res.step_idx = query.step_idx;
	res.char_selection = query.char_selection;
	res.char_color_selection = query.char_color_selection;
	memcpy(res.stage_selections, query.stage_selections, 2);

	// DIAGNOSTIC. Which step is which.
	//
	// The draft negotiates through here - it completes a step and fetches the
	// opponent's - so every ban, every stage and every character passes this
	// point with an index on it. What that index MEANS is the one thing needed
	// before a random-stage room can tell the draft to skip its stage steps,
	// and it is not written down anywhere. So it gets read off a real draft
	// rather than assumed, which is what went wrong twice already.
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] draft step %d done: char %d colour %d stages %d,%d",
	         query.step_idx, query.char_selection, query.char_color_selection,
	         query.stage_selections[0], query.stage_selections[1]);

	// Our own step landed, so whatever was driving the pad for it stops. This is
	// the "when to stop" that a timer could never get right.
	draft_last_local_step = query.step_idx;
	draft_drive_armed = false;

	// ⚠️ A watcher has no business in somebody else's draft. It should never
	// reach this screen - the room sends it straight to the splash - but its
	// netplay client points at the two people it is watching, so if it ever did,
	// this would inject a stranger's bans and picks into their set.
	if (isWatching())
		return;

	if (slippi_netplay)
		slippi_netplay->SendGamePrepStep(res);
}

void CEXISlippi::prepareGamePrepOppStep(const SlippiExiTypes::GpFetchStepQuery &query)
{
	SlippiExiTypes::GpFetchStepResponse resp;

	m_read_queue.clear();

	// Start by indicating not found
	resp.is_found = false;

#ifdef LOCAL_TESTING
	static int delay_count = 0;

	delay_count++;
	if (delay_count >= 90)
	{
		resp.is_found = true;
		resp.is_skip = true; // Will make client just pick the next available options

		delay_count = 0;
	}
#else
	// ⚠ EVERY ask, not only the ones that can be answered. A client only ever
	// asks about a step it is NOT performing, so this is the draft telling us
	// whose turn it is - and the ones that go unanswered say it FIRST, which is
	// when it is still worth knowing.
	draft_fetch_step = query.step_idx;

	SlippiGamePrepStepResults res;
	if (slippi_netplay && slippi_netplay->GetGamePrepResults(query.step_idx, res))
	{
		// If we have received a response from the opponent, prepare the values for response
		resp.is_found = true;
		resp.is_skip = false;

		// The opponent's step is in hand. This is the only safe place to notice
		// that: GetGamePrepResults discards anything it passes over, so nothing
		// else may go looking.
		if ((int)query.step_idx > draft_opp_step_done)
			draft_opp_step_done = query.step_idx;
		resp.char_selection = res.char_selection;
		resp.char_color_selection = res.char_color_selection;
		memcpy(resp.stage_selections, res.stage_selections, 2);

		// DIAGNOSTIC, and ONCE PER STEP rather than per call - the draft asks
		// this every frame until an answer arrives, so logging every call would
		// bury the thing being looked for.
		static int said_step = -1;
		if (query.step_idx != said_step)
		{
			said_step = query.step_idx;
			WARN_LOG(SLIPPI_ONLINE,
			         "[Rooms] draft step %d from opponent: char %d colour %d stages %d,%d",
			         query.step_idx, res.char_selection, res.char_color_selection,
			         res.stage_selections[0], res.stage_selections[1]);
		}
	}
#endif

	auto data_ptr = (u8 *)&resp;
	m_read_queue.insert(m_read_queue.end(), data_ptr, data_ptr + sizeof(SlippiExiTypes::GpFetchStepResponse));
}

void CEXISlippi::handleCompleteSet(const SlippiExiTypes::ReportSetCompletionQuery &query)
{
	auto lastMatchId = recentMmResult.id;
	if (lastMatchId.find("mode.ranked") != std::string::npos)
	{
		INFO_LOG(SLIPPI_ONLINE, "Reporting set completion: %s", lastMatchId.c_str());

		auto userInfo = user->GetUserInfo();

		auto status = query.endMode == 0 ? "normal_completion" : "abnormal_completion";
		slprs_exi_device_report_match_status(slprs_exi_device_ptr, lastMatchId.c_str(), status, true);
	}
}

void CEXISlippi::handleMatchStatusUpdate(const SlippiExiTypes::ReportMatchStatusUpdateQuery &query)
{
	auto lastMatchId = recentMmResult.id;
	if (lastMatchId.find("mode.ranked") == std::string::npos)
	{
		return; // Only report match status updates for ranked matches
	}

	auto statusMapRes = statusIdxMap.find(query.statusIdx);
	if (statusMapRes == statusIdxMap.end())
	{
		ERROR_LOG(SLIPPI_ONLINE, "Invalid status index: %d", query.statusIdx);
		return; // Invalid status index
	}

	auto statusString = statusMapRes->second;

	INFO_LOG(SLIPPI_ONLINE, "Reporting match status update: %s, Status: %s", lastMatchId.c_str(), statusString.c_str());

	// Report asynchronously when called from the game
	slprs_exi_device_report_match_status(slprs_exi_device_ptr, lastMatchId.c_str(), statusString.c_str(), true);
}

void CEXISlippi::handleGetPlayerSettings()
{
	m_read_queue.clear();

	SlippiExiTypes::GetPlayerSettingsResponse resp = {};

	std::vector<std::vector<std::string>> messagesByPlayer = {{}, {}, {}, {}};

	// These chat messages will be used when previewing messages
	auto userChatMessages = user->GetUserChatMessages();
	if (userChatMessages.size() == 16)
	{
		messagesByPlayer[0] = userChatMessages;
	}

	// These chat messages will be set when we have an opponent. We load their and our messages
	auto playerInfo = matchmaking->GetPlayerInfo();
	for (auto &player : playerInfo)
	{
		messagesByPlayer[player.port - 1] = player.chatMessages;
	}

	for (int i = 0; i < 4; i++)
	{
		// If any of the users in the chat messages vector have a payload that is incorrect,
		// force that player to the default chat messages. A valid payload is 16 entries.
		if (messagesByPlayer[i].size() != 16)
		{
			messagesByPlayer[i] = user->GetDefaultChatMessages();
		}

		for (int j = 0; j < 16; j++)
		{
			auto str = ConvertStringForGame(messagesByPlayer[i][j], MAX_MESSAGE_LENGTH);
			sprintf(resp.settings[i].chatMessages[j], "%s", str.c_str());
		}
	}

	auto data_ptr = (u8 *)&resp;
	m_read_queue.insert(m_read_queue.end(), data_ptr, data_ptr + sizeof(SlippiExiTypes::GetPlayerSettingsResponse));
}

void CEXISlippi::handleGetRank()
{
	RustRankInfo rank_info = slprs_get_rank_info(slprs_exi_device_ptr);
	m_read_queue.clear();

	// Determine rank info visibility
	u8 local_rank_enabled = static_cast<u8>(SConfig::GetInstance().bSlippiPlayerRankDisplay);
	u8 opp_rank_enabled = static_cast<u8>(SConfig::GetInstance().bSlippiOpponentRankDisplay);
	u8 rank_visibility = local_rank_enabled | (opp_rank_enabled << 1);

	// Push rank data header
	m_read_queue.push_back(rank_visibility);
	m_read_queue.push_back(static_cast<u8>(rank_info.fetch_status));

	// ERROR_LOG_FMT(SLIPPI_ONLINE, "Update count: {}", rank_info.rating_update_count);

	// Push rank data
	m_read_queue.push_back(static_cast<u8>(rank_info.rank));
	appendWordToBuffer(&m_read_queue, *(u32 *)(&rank_info.rating_ordinal));
	appendWordToBuffer(&m_read_queue, static_cast<u32>(rank_info.rating_update_count));
	appendWordToBuffer(&m_read_queue, *(u32 *)(&rank_info.rating_change));
	m_read_queue.push_back(static_cast<u8>(rank_info.rank_change));
}

void CEXISlippi::DMAWrite(u32 _uAddr, u32 _uSize)
{
	u8 *memPtr = Memory::GetPointer(_uAddr);
	// INFO_LOG(SLIPPI, "DMA Write: %x, Size: %d", _uAddr, _uSize);

	u32 bufLoc = 0;

	if (memPtr == nullptr)
	{
		NOTICE_LOG(SLIPPI, "DMA Write was passed an invalid address: %x", _uAddr);
		Dolphin_Debugger::PrintCallstack(LogTypes::LOG_TYPE::SLIPPI, LogTypes::LOG_LEVELS::LNOTICE);
		m_read_queue.clear();
		return;
	}

	u8 byte = memPtr[0];
	if (byte == CMD_RECEIVE_COMMANDS)
	{
		time(&gameStartTime); // Store game start time
		u8 receiveCommandsLen = memPtr[1];
		configureCommands(&memPtr[1], receiveCommandsLen);
		writeToFileAsync(&memPtr[0], receiveCommandsLen + 1, "create");
		bufLoc += receiveCommandsLen + 1;
		g_needInputForFrame = true;

		m_slippiserver->startGame();
		m_slippiserver->write(&memPtr[0], receiveCommandsLen + 1);

		slprs_exi_device_reporter_push_replay_data(slprs_exi_device_ptr, &memPtr[0], receiveCommandsLen + 1);
	}

	if (byte == CMD_MENU_FRAME)
	{
		m_slippiserver->write(&memPtr[0], _uSize);
		g_needInputForFrame = true;
		return;
	}

	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMAWrite: addr: 0x%08x size: %d, bufLoc:[%02x %02x %02x %02x %02x]",
	         _uAddr, _uSize, memPtr[bufLoc], memPtr[bufLoc + 1], memPtr[bufLoc + 2], memPtr[bufLoc + 3],
	         memPtr[bufLoc + 4]);

	u8 prevCommandByte = 0;

	while (bufLoc < _uSize)
	{
		byte = memPtr[bufLoc];
		// INFO_LOG(SLIPPI, "EXI SLIPPI: Loc: %d, Size: %d, Cmd: 0x%x", bufLoc, _uSize, byte);
		if (!payloadSizes.count(byte))
		{
			// This should never happen. Do something else if it does?
			ERROR_LOG(SLIPPI, "EXI SLIPPI: Invalid command byte: 0x%X. Prev command: 0x%X", byte, prevCommandByte);
			return;
		}

		u32 payloadLen = payloadSizes[byte];
		switch (byte)
		{
		case CMD_RECEIVE_GAME_END:
			writeToFileAsync(&memPtr[bufLoc], payloadLen + 1, "close");
			m_slippiserver->write(&memPtr[bufLoc], payloadLen + 1);
			m_slippiserver->endGame();
			slprs_exi_device_reporter_push_replay_data(slprs_exi_device_ptr, &memPtr[bufLoc], payloadLen + 1);
			break;
		case CMD_PREPARE_REPLAY:
			// log.open("log.txt");
			prepareGameInfo(&memPtr[bufLoc + 1]);
			break;
		case CMD_READ_FRAME:
			prepareFrameData(&memPtr[bufLoc + 1]);
			break;
		case CMD_FRAME_BOOKEND:
			g_needInputForFrame = true;
			writeToFileAsync(&memPtr[bufLoc], payloadLen + 1, "");
			m_slippiserver->write(&memPtr[bufLoc], payloadLen + 1);
			slprs_exi_device_reporter_push_replay_data(slprs_exi_device_ptr, &memPtr[bufLoc], payloadLen + 1);
			break;
		case CMD_IS_STOCK_STEAL:
			prepareIsStockSteal(&memPtr[bufLoc + 1]);
			break;
		case CMD_IS_FILE_READY:
			prepareIsFileReady();
			break;
		case CMD_GET_GECKO_CODES:
			m_read_queue.clear();
			m_read_queue.insert(m_read_queue.begin(), geckoList.begin(), geckoList.end());
			break;
		case CMD_ONLINE_INPUTS:
			handleOnlineInputs(&memPtr[bufLoc + 1]);
			break;
		case CMD_CAPTURE_SAVESTATE:
			handleCaptureSavestate(&memPtr[bufLoc + 1]);
			break;
		case CMD_LOAD_SAVESTATE:
			handleLoadSavestate(&memPtr[bufLoc + 1]);
			break;
		case CMD_GET_MATCH_STATE:
			prepareOnlineMatchState();
			break;
		case CMD_FIND_OPPONENT:
			startFindMatch(&memPtr[bufLoc + 1]);
			break;
		case CMD_SET_MATCH_SELECTIONS:
			setMatchSelections(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_CREATE:
			handleRoomCreate(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_QUEUE:
			handleRoomQueue(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_STAGE_DRAFT:
			handleRoomStageDraft(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_DRAFT_DRIVE:
			prepareRoomDraftDrive();
			break;
		case CMD_ROOM_LEAVE_TRAIN:
			prepareRoomLeaveTraining();
			break;
		case CMD_ROOM_LEAVE:
			handleRoomLeave(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_JOIN:
			handleRoomJoin(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_STATE:
			prepareRoomState();
			break;
		case CMD_ROOM_LIST:
			handleRoomList(&memPtr[bufLoc + 1]);
			break;
		case CMD_ROOM_WATCH:
			handleRoomWatch();
			break;
		case CMD_ROOM_LIST_READ:
			prepareRoomList();
			break;
		case CMD_FILE_LENGTH:
			prepareFileLength(&memPtr[bufLoc + 1]);
			break;
		case CMD_FETCH_CODE_SUGGESTION:
			handleNameEntryLoad(&memPtr[bufLoc + 1]);
			break;
		case CMD_FILE_LOAD:
			prepareFileLoad(&memPtr[bufLoc + 1]);
			break;
		case CMD_PREMADE_TEXT_LENGTH:
			preparePremadeTextLength(&memPtr[bufLoc + 1]);
			break;
		case CMD_PREMADE_TEXT_LOAD:
			preparePremadeTextLoad(&memPtr[bufLoc + 1]);
			break;
		case CMD_OPEN_LOGIN:
			handleLogInRequest();
			break;
		case CMD_LOGOUT:
			handleLogOutRequest();
			break;
		case CMD_GET_ONLINE_STATUS:
			prepareOnlineStatus();
			break;
		case CMD_CLEANUP_CONNECTION:
			handleConnectionCleanup();
			break;
		case CMD_LOG_MESSAGE:
			logMessageFromGame(&memPtr[bufLoc + 1]);
			break;
		case CMD_SEND_CHAT_MESSAGE:
			handleChatMessage(&memPtr[bufLoc + 1]);
			break;
		case CMD_UPDATE:
			handleUpdateAppRequest();
			break;
		case CMD_GET_NEW_SEED:
			prepareNewSeed();
			break;
		case CMD_REPORT_GAME:
			handleReportGame(SlippiExiTypes::Convert<SlippiExiTypes::ReportGameQuery>(&memPtr[bufLoc]));
			break;
		case CMD_GCT_LENGTH:
			prepareGctLength();
			break;
		case CMD_GCT_LOAD:
			prepareGctLoad(&memPtr[bufLoc + 1]);
			ConfigureJukebox();
			break;
		case CMD_GET_DELAY:
			prepareDelayResponse();
			break;
		case CMD_OVERWRITE_SELECTIONS:
			handleOverwriteSelections(
			    SlippiExiTypes::Convert<SlippiExiTypes::OverwriteSelectionsQuery>(&memPtr[bufLoc]));
			break;
		case CMD_GP_FETCH_STEP:
			prepareGamePrepOppStep(SlippiExiTypes::Convert<SlippiExiTypes::GpFetchStepQuery>(&memPtr[bufLoc]));
			break;
		case CMD_GP_COMPLETE_STEP:
			handleGamePrepStepComplete(SlippiExiTypes::Convert<SlippiExiTypes::GpCompleteStepQuery>(&memPtr[bufLoc]));
			break;
		case CMD_REPORT_SET_COMPLETE:
			handleCompleteSet(SlippiExiTypes::Convert<SlippiExiTypes::ReportSetCompletionQuery>(&memPtr[bufLoc]));
			break;
		case CMD_REPORT_MATCH_STATUS_UPDATE:
			handleMatchStatusUpdate(
			    SlippiExiTypes::Convert<SlippiExiTypes::ReportMatchStatusUpdateQuery>(&memPtr[bufLoc]));
			break;
		case CMD_GET_PLAYER_SETTINGS:
			handleGetPlayerSettings();
			break;
		case CMD_PLAY_MUSIC:
		{
			auto args = SlippiExiTypes::Convert<SlippiExiTypes::PlayMusicQuery>(&memPtr[bufLoc]);
			slprs_jukebox_start_song(slprs_exi_device_ptr, args.offset, args.size);
			break;
		}
		case CMD_STOP_MUSIC:
			slprs_jukebox_stop_music(slprs_exi_device_ptr);
			break;
		case CMD_CHANGE_MUSIC_VOLUME:
		{
			auto args = SlippiExiTypes::Convert<SlippiExiTypes::ChangeMusicVolumeQuery>(&memPtr[bufLoc]);
			slprs_jukebox_set_melee_music_volume(slprs_exi_device_ptr, args.volume);
			break;
		}
		case CMD_GET_RANK:
		{
			handleGetRank();
			break;
		}
		case CMD_FETCH_RANK:
		{
			slprs_fetch_match_result(slprs_exi_device_ptr, recentMmResult.id.c_str());
			break;
		}
		default:
			writeToFileAsync(&memPtr[bufLoc], payloadLen + 1, "");
			m_slippiserver->write(&memPtr[bufLoc], payloadLen + 1);
			slprs_exi_device_reporter_push_replay_data(slprs_exi_device_ptr, &memPtr[bufLoc], payloadLen + 1);
			break;
		}

		prevCommandByte = byte;
		bufLoc += payloadLen + 1;
	}
}

void CEXISlippi::DMARead(u32 addr, u32 size)
{
	if (m_read_queue.empty())
	{
		ERROR_LOG(SLIPPI, "EXI SLIPPI DMARead: Empty");
		return;
	}

	m_read_queue.resize(size, 0); // Resize response array to make sure it's all full/allocated

	auto queueAddr = &m_read_queue[0];
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMARead: addr: 0x%08x size: %d, startResp: [%02x %02x %02x %02x %02x]",
	         addr, size, queueAddr[0], queueAddr[1], queueAddr[2], queueAddr[3], queueAddr[4]);

	// Copy buffer data to memory
	Memory::CopyToEmu(addr, queueAddr, size);
}

// Configures (or reconfigures) the Jukebox by calling over the C FFI boundary.
//
// This method can also be called, indirectly, from the Settings panel.
void CEXISlippi::ConfigureJukebox()
{
#ifndef IS_PLAYBACK
	// Exclusive WASAPI and the Jukebox do not play nicely, so we just don't bother enabling
	// the Jukebox in that scenario - why bother doing the processing work when it's not even
	// possible to play it?
	// Jukebox will also respect no audio output
	std::string backend = SConfig::GetInstance().sBackend;
	if (backend.find(BACKEND_EXCLUSIVE_WASAPI) != std::string::npos ||
	    backend.find(BACKEND_NULLSOUND) != std::string::npos)
	{
		return;
	}

	bool jukeboxEnabled = SConfig::GetInstance().bSlippiJukeboxEnabled;
	int systemVolume = SConfig::GetInstance().m_IsMuted ? 0 : SConfig::GetInstance().m_Volume;
	int jukeboxVolume = SConfig::GetInstance().iSlippiJukeboxVolume;

	slprs_exi_device_configure_jukebox(slprs_exi_device_ptr, jukeboxEnabled, systemVolume, jukeboxVolume);
#endif
}

void CEXISlippi::SetJukeboxDolphinSystemVolume()
{
	int systemVolume = SConfig::GetInstance().m_IsMuted ? 0 : SConfig::GetInstance().m_Volume;
	slprs_jukebox_set_dolphin_system_volume(slprs_exi_device_ptr, systemVolume);
}

void CEXISlippi::SetJukeboxDolphinMusicVolume()
{
	int jukeboxVolume = SConfig::GetInstance().iSlippiJukeboxVolume;
	slprs_jukebox_set_dolphin_music_volume(slprs_exi_device_ptr, jukeboxVolume);
}

bool CEXISlippi::IsPresent() const
{
	return true;
}

void CEXISlippi::TransferByte(u8 &byte) {}
