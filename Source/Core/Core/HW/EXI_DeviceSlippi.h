// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <SlippiLib/SlippiGame.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Core/HW/EXI_Device.h"
#include "Core/Slippi/SlippiDirectCodes.h"
#include "Core/Slippi/SlippiWatch.h"
#include "Core/Slippi/SlippiExiTypes.h"
#include "Core/Slippi/SlippiGameFileLoader.h"
#include "Core/Slippi/SlippiMatchmaking.h"
#include "Core/Slippi/SlippiNetplay.h"
#include "Core/Slippi/SlippiReplayComm.h"
#include "Core/Slippi/SlippiSavestate.h"
#include "Core/Slippi/SlippiSpectate.h"
#include "Core/Slippi/SlippiUser.h"

#define MAX_NAME_LENGTH 15

// Rooms: the shape of the CMD_ROOM_STATE reply.
//
// ⚠️ This layout is duplicated, by hand, in the game module's rooms.h. There is
// no generator and no shared header - one side is C++ compiled for the host,
// the other is PowerPC compiled for Melee - so a field added here and not there
// reads silently wrong bytes rather than failing to build. Change both.
//
// Fixed size on purpose: the module reads it into a struct at a known offset
// and a variable-length reply would need a parser on the PowerPC side.
#define ROOM_CODE_LEN       4    // rooms are four characters, e.g. 8NXU
#define ROOM_STATE_NAME_LEN 32   // ConvertStringForGame gives 31, padded to 32
#define ROOM_STATE_MAX_QUEUE 6
#define ROOM_STATE_MAX_LOBBY 6
#define ROOM_STATE_NAMES (2 + ROOM_STATE_MAX_QUEUE + ROOM_STATE_MAX_LOBBY)
/* Which PORT bans first in the draft, in a byte the header already had spare.
 *
 * Melee's counterpick rule is that the winner bans, and GPDO_PREV_WINNER is the
 * byte the draft reads it from - proved by forcing it and watching the ban move
 * to the other player. A room zeroes its whole GamePrepData, so port 0 banned
 * every time and whoever made the room watched their opponent ban.
 *
 * In a room the winner is the one who STAYED, which is the same person the
 * pairing calls its host: pd_pairings picks its host as the first still in the
 * queue, tie-broken by who joined the room first. For a first game, and for one
 * whose winner has left, that falls through to the longest-standing member. */
#define ROOM_STATE_BAN_FIRST 0x09

/* The room's own settings, in the next byte the header had spare.
 *
 * ⚠ Two things, not one: whether the room drafts its stages, and whether the
 * player reading this is the one allowed to change that. The screen needs both
 * - everybody sees the setting, one person gets told they can press a button -
 * and the owner can change hands while people are standing in the room. */
/* What the pad should do in a random-stage room's draft. One byte, asked once a
 * frame; the frame counting is Dolphin's so the game side keeps no state. */
#define ROOM_DRIVE_NOTHING 0
#define ROOM_DRIVE_SWEEP   1   /* hold the cursor moving along the stage row */
#define ROOM_DRIVE_PRESS   2   /* press A */
#define ROOM_DRIVE_PRESS_FRAMES 3
/* ⚠ A stage is not chosen by one press. The first A puts the mark on
 * the icon and raises an OK/Redo panel with OK already highlighted; the
 * choice is not made until OK is pressed too. The first build drove the
 * ban perfectly and then sat on that panel forever.
 *
 * The gap is a RELEASE, not politeness: two presses with no frames between
 * them are one held button, and the panel would never see a second press. */
#define ROOM_DRIVE_GAP_FRAMES 20
/* How long to listen before concluding that step 0 is OURS.
 *
 * ⚠ The client that does NOT perform step 0 starts asking for it at once,
 * every frame. The client that DOES perform it never asks for it at all - so
 * silence is the answer, and silence needs a length. Two seconds is far longer
 * than the first ask takes to arrive and far shorter than the draft's clock. */
#define ROOM_DRIVE_LISTEN_FRAMES 120

#define ROOM_STATE_SETTINGS 0x0A
#define ROOM_SETTING_DRAFT  0x01   /* stages are drafted, not random */

/* ROOM_STATE_MM: what Slippi's matchmaking is doing about the pairing the
 * room asked it for. ⚠️ The room used to have no way to ask. It sent
 * CMD_FIND_OPPONENT and then waited for ROOM_FLAG_CONNECTED, which only ever
 * arrives on success - so a search that failed left the screen sitting on a
 * pairing forever with nothing said and nothing to do. */
#define ROOM_MM_FAILED    0x01   /* Slippi gave up on this search */
#define ROOM_MM_SEARCHING 0x02   /* still trying */
#define ROOM_SETTING_OWNER  0x02   /* ...and you are the one who may say so */

#define ROOM_STATE_HEADER 12
#define ROOM_STATE_NAMES_END (ROOM_STATE_HEADER + ROOM_STATE_NAMES * ROOM_STATE_NAME_LEN)

// The opponent's connect code, Shift-JIS, ready to go straight into a
// CMD_FIND_OPPONENT payload without the game converting anything. 18 bytes is
// what that command takes; the extra two keep the struct aligned.
#define ROOM_STATE_OPPCODE ROOM_STATE_NAMES_END
#define ROOM_STATE_OPPCODE_LEN 20

// How many crowns each of those names has, in the SAME order and the same
// count - two playing, then the queue, then the lobby. One byte each, capped,
// because a crown count past 255 is not a thing that needs drawing.
#define ROOM_STATE_CROWNS (ROOM_STATE_OPPCODE + ROOM_STATE_OPPCODE_LEN)

// The room's own code, and the passcode of a private one. Fixed width, blank
// padded, so the module indexes rather than parses. The passcode is empty for
// a public room - those are listed and have none.
#define ROOM_STATE_CODE   (ROOM_STATE_CROWNS + ROOM_STATE_NAMES)
#define ROOM_STATE_CODE_LEN 8
#define ROOM_STATE_PASS   (ROOM_STATE_CODE + ROOM_STATE_CODE_LEN)
#define ROOM_STATE_PASS_LEN 8

#define ROOM_STATE_SIZE (ROOM_STATE_PASS + ROOM_STATE_PASS_LEN)

// Bit 2 of the flags byte: the pairing is on and the game should go and
// connect. Distinct from PLAYING, which means a match is already under way.
#define ROOM_FLAG_READY 0x04

// Bit 4: Slippi says the two are actually CONNECTED.
//
// ⚠️ The handoff to the draft waits on this and not a moment earlier. The
// screens past it fork on the same state, and anything below CONNECTION_SUCCESS
// lands them in their "searching" branch - where a character cannot be locked
// in. Handing over early means arriving at a screen that refuses to start.
#define ROOM_FLAG_CONNECTED 0x08

// We are IN a room - we have a code and the heartbeat is running - as opposed
// to browsing the public list.
//
// ⚠ Deliberately not the same question as ROOM_FLAG_VALID. Valid means a tick
// has come BACK, which is half a second after the scene is built, and the room
// screen used to decide which of its two screens it was from that. So a room
// you had just made drew the public list until its first tick landed, and the
// public list drew a room. Two half-second flashes of the wrong screen, one
// cause.
#define ROOM_FLAG_INROOM 0x10

// A watch is up and we have enough of the match to describe it.
//
// ⚠ Not "a watch was asked for". The room screen hands Melee over on this, and
// arriving before both players have said what they picked means a match with no
// characters, no stage and no seed.
#define ROOM_FLAG_WATCHING 0x20

// 0xFF rather than 0 for "not picked": 0 is Captain Falcon and a real stage.
// Bit 6: this room is PRIVATE - unlisted, and it has a passcode. The screen
// stars the code and the passcode out until somebody holds R or L.
#define ROOM_FLAG_PRIVATE 0x40

// Bit 7: we have asked to be in the queue. See the note where it is set -
// the position field cannot answer this and never could.
#define ROOM_FLAG_QUEUED 0x80

#define ROOM_NOT_PICKED 0xFF

// Rooms: the shape of the CMD_ROOM_LIST_READ reply. Duplicated by hand in the
// module's rooms.h, exactly like the room state above - change both.
//
//   +0x00  u8  flags   bit0 = a fetch has come back at all
//   +0x01  u8  count
//   +0x02  u8  pad[2]
//   +0x04  rooms, 48 bytes each:
//            +0x00  char code[8]    four characters, null-terminated
//            +0x08  u8   mode       index into the menu's kinds, 0xFF unknown
//            +0x09  u8   players
//            +0x0A  u8   capacity   the room's own limit
//            +0x0B  u8   pad
//            +0x0C  char owner[32]  who opened it
//            +0x2C  u8   pad[4]
#define ROOM_LIST_MAX     8
#define ROOM_LIST_STRIDE  48
#define ROOM_LIST_HEADER  4
#define ROOM_LIST_SIZE    (ROOM_LIST_HEADER + ROOM_LIST_MAX * ROOM_LIST_STRIDE)
#define ROOM_LIST_FETCHED 0x01
#define ROOM_MODE_UNKNOWN 0xFF
#define MAX_MESSAGE_LENGTH 25
#define CONNECT_CODE_LENGTH 8

extern bool g_needInputForFrame;

// Emulated Slippi device used to receive and respond to in-game messages
class CEXISlippi : public IEXIDevice
{
  public:
	CEXISlippi();
	virtual ~CEXISlippi();

	void DMAWrite(u32 _uAddr, u32 _uSize) override;
	void DMARead(u32 addr, u32 size) override;

	void ConfigureJukebox();
	void SetJukeboxDolphinSystemVolume();
	void SetJukeboxDolphinMusicVolume();

	bool IsPresent() const override;

  private:
	enum
	{
		CMD_UNKNOWN = 0x0,

		// Recording
		CMD_RECEIVE_COMMANDS = 0x35,
		CMD_RECEIVE_GAME_INFO = 0x36,
		CMD_RECEIVE_POST_FRAME_UPDATE = 0x38,
		CMD_RECEIVE_GAME_END = 0x39,
		CMD_RECEIVE_INITIAL_RNG = 0x3A,
		CMD_RECEIVE_ITEM = 0x3B,
		CMD_FRAME_BOOKEND = 0x3C,
		CMD_GECKO_LIST = 0x3D,
		CMD_MENU_FRAME = 0x3E,
		CMD_RECEIVE_FOD_INFO = 0x3F,
		CMD_RECEIVE_DL_INFO = 0x40,
		CMD_RECEIVE_PS_INFO = 0x41,

		CMD_RECEIVE_BONES = 0x60,

		// Playback
		CMD_PREPARE_REPLAY = 0x75,
		CMD_READ_FRAME = 0x76,
		CMD_GET_LOCATION = 0x77,
		CMD_IS_FILE_READY = 0x88,
		CMD_IS_STOCK_STEAL = 0x89,
		CMD_GET_GECKO_CODES = 0x8A,

		// Online
		CMD_ONLINE_INPUTS = 0xB0,
		CMD_CAPTURE_SAVESTATE = 0xB1,
		CMD_LOAD_SAVESTATE = 0xB2,
		CMD_GET_MATCH_STATE = 0xB3,
		CMD_FIND_OPPONENT = 0xB4,
		CMD_SET_MATCH_SELECTIONS = 0xB5,
		CMD_OPEN_LOGIN = 0xB6,
		CMD_LOGOUT = 0xB7,
		CMD_UPDATE = 0xB8,
		CMD_GET_ONLINE_STATUS = 0xB9,
		CMD_CLEANUP_CONNECTION = 0xBA,
		CMD_SEND_CHAT_MESSAGE = 0xBB,
		CMD_GET_NEW_SEED = 0xBC,
		CMD_REPORT_GAME = 0xBD,
		CMD_FETCH_CODE_SUGGESTION = 0xBE,
		CMD_OVERWRITE_SELECTIONS = 0xBF,
		CMD_GP_COMPLETE_STEP = 0xC0,
		CMD_GP_FETCH_STEP = 0xC1,
		CMD_REPORT_SET_COMPLETE = 0xC2,
		CMD_GET_PLAYER_SETTINGS = 0xC3,
		CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4,

		// Rooms: its own rooms. 0xC5 up is clear of everything Slippi uses -
		// their ids run to 0xC4 and then resume at 0xD1.
		CMD_ROOM_CREATE = 0xC5,
		CMD_ROOM_QUEUE = 0xC6,  // pressed Start, or stepped back out
		CMD_ROOM_JOIN = 0xC8,   // a room code, from the browser or typed
		CMD_ROOM_STATE = 0xC9,  // read back: everything the room screen draws
		CMD_ROOM_LIST = 0xC7,   // go and fetch the public rooms
		CMD_ROOM_LIST_READ = 0xCA, // read back what the fetch found
		// Watch the match the room is playing. No payload - Dolphin already
		// holds the room state, and with it both players' addresses.
		CMD_ROOM_WATCH = 0xCB,
		CMD_ROOM_LEAVE = 0xCC,  // hold B: out of the room altogether
		CMD_ROOM_STAGE_DRAFT = 0xCD, // the owner turning the stage draft on or off
		CMD_ROOM_DRAFT_DRIVE = 0xCE, // read back: what the pad should do in the draft

		// Misc
		CMD_LOG_MESSAGE = 0xD0,
		CMD_FILE_LENGTH = 0xD1,
		CMD_FILE_LOAD = 0xD2,
		CMD_GCT_LENGTH = 0xD3,
		CMD_GCT_LOAD = 0xD4,
		CMD_GET_DELAY = 0xD5,
		CMD_PLAY_MUSIC = 0xD6,
		CMD_STOP_MUSIC = 0xD7,
		CMD_CHANGE_MUSIC_VOLUME = 0xD8,
		CMD_PREMADE_TEXT_LENGTH = 0xE1,
		CMD_PREMADE_TEXT_LOAD = 0xE2,
		CMD_GET_RANK = 0xE3,
		CMD_FETCH_RANK = 0xE4,
		CMD_GET_RANK_VISIBILITY = 0xE5
	};

	enum
	{
		FRAME_RESP_WAIT = 0,
		FRAME_RESP_CONTINUE = 1,
		FRAME_RESP_TERMINATE = 2,
		FRAME_RESP_FASTFORWARD = 3,
	};

	// This is a mapping of u8s to status updates such that we dont have to send
	// strings from the game
	// clang-format off
	std::unordered_map<u8, std::string> statusIdxMap = {
		{1, "connecting"},
		{10, "game_setup_1"},
		{11, "game_setup_2"},
		{12, "game_setup_3"},
		{13, "game_setup_4"}, // These are just here if we ever have longer than bo3s
		{14, "game_setup_5"},
		{15, "game_setup_6"},
		{16, "game_setup_7"}, // Surely we never have more than bo7s
		{20, "game_start_1"},
		{21, "game_start_2"},
		{22, "game_start_3"},
		{23, "game_start_4"},
		{24, "game_start_5"},
		{25, "game_start_6"},
		{26, "game_start_7"},
		{30, "normal_completion"},
		{31, "abnormal_completion"},
		{40, "abandoned"},
	};
	// clang-format on

	std::unordered_map<u8, u32> payloadSizes = {
	    // The actual size of this command will be sent in one byte
	    // after the command is received. The other receive command IDs
	    // and sizes will be received immediately following
	    {CMD_RECEIVE_COMMANDS, 1},

	    // The following are all commands used to play back a replay and
	    // have fixed sizes unless otherwise specified
	    {CMD_PREPARE_REPLAY, 0xFFFF}, // Variable size... will only work if by itself
	    {CMD_READ_FRAME, 4},
	    {CMD_IS_STOCK_STEAL, 5},
	    {CMD_GET_LOCATION, 6},
	    {CMD_IS_FILE_READY, 0},
	    {CMD_GET_GECKO_CODES, 0},

	    // The following are used for Slippi online and also have fixed sizes
	    {CMD_ONLINE_INPUTS, 25},
	    {CMD_CAPTURE_SAVESTATE, 32},
	    {CMD_LOAD_SAVESTATE, 32},
	    {CMD_GET_MATCH_STATE, 0},
	    {CMD_FIND_OPPONENT, 19},
	    {CMD_SET_MATCH_SELECTIONS, 9},
	    {CMD_SEND_CHAT_MESSAGE, 2},
	    {CMD_OPEN_LOGIN, 0},
	    {CMD_LOGOUT, 0},
	    {CMD_UPDATE, 0},
	    {CMD_GET_ONLINE_STATUS, 0},
	    {CMD_CLEANUP_CONNECTION, 0},
	    {CMD_GET_NEW_SEED, 0},
	    {CMD_REPORT_GAME, static_cast<u32>(sizeof(SlippiExiTypes::ReportGameQuery) - 1)},
	    {CMD_FETCH_CODE_SUGGESTION, 31},
	    {CMD_OVERWRITE_SELECTIONS, static_cast<u32>(sizeof(SlippiExiTypes::OverwriteSelectionsQuery) - 1)},
	    {CMD_GP_COMPLETE_STEP, static_cast<u32>(sizeof(SlippiExiTypes::GpCompleteStepQuery) - 1)},
	    {CMD_GP_FETCH_STEP, static_cast<u32>(sizeof(SlippiExiTypes::GpFetchStepQuery) - 1)},
	    {CMD_REPORT_SET_COMPLETE, static_cast<u32>(sizeof(SlippiExiTypes::ReportSetCompletionQuery) - 1)},
	    {CMD_GET_PLAYER_SETTINGS, 0},
	    {CMD_REPORT_MATCH_STATUS_UPDATE, static_cast<u32>(sizeof(SlippiExiTypes::ReportMatchStatusUpdateQuery) - 1)},

	    // Misc
	    // Rooms: mode byte, then listed/unlisted.
	    {CMD_ROOM_CREATE, 0x2},
	    {CMD_ROOM_QUEUE, 0x1},        // one byte: queued or not
	    {CMD_ROOM_JOIN, ROOM_CODE_LEN},
	    {CMD_ROOM_STATE, 0x0},        // asks for the reply, sends nothing
	    {CMD_ROOM_LIST, 0x1},         // one byte: which mode, 0xFF for any
	    {CMD_ROOM_LIST_READ, 0x0},
	    {CMD_ROOM_WATCH, 0x0},
	    {CMD_ROOM_LEAVE, 0x1},        // one byte: were we IN a room, or just browsing
	    {CMD_ROOM_STAGE_DRAFT, 0x1},  // one byte: the setting we want
	    {CMD_ROOM_DRAFT_DRIVE, 0x0},  // no payload - the answer is one byte

	    {CMD_LOG_MESSAGE, 0xFFFF}, // Variable size... will only work if by itself
	    {CMD_FILE_LENGTH, 0x40},
	    {CMD_FILE_LOAD, 0x40},
	    {CMD_GCT_LENGTH, 0x0},
	    {CMD_GCT_LOAD, 0x4},
	    {CMD_GET_DELAY, 0x0},
	    {CMD_PLAY_MUSIC, static_cast<u32>(sizeof(SlippiExiTypes::PlayMusicQuery) - 1)},
	    {CMD_STOP_MUSIC, 0x0},
	    {CMD_CHANGE_MUSIC_VOLUME, static_cast<u32>(sizeof(SlippiExiTypes::ChangeMusicVolumeQuery) - 1)},
	    {CMD_PREMADE_TEXT_LENGTH, 0x2},
	    {CMD_PREMADE_TEXT_LOAD, 0x2},
	    {CMD_GET_RANK, 0x0},
	    {CMD_FETCH_RANK, 0x0},
	    {CMD_GET_RANK_VISIBILITY, 0x0},
	};

	struct WriteMessage
	{
		std::vector<u8> data;
		std::string operation;
	};

	// A pointer to a "shadow" EXI Device that lives on the Rust side of things.
	// This should be cleaned up in any destructor!
	uintptr_t slprs_exi_device_ptr;

	// .slp File creation stuff
	u32 writtenByteCount = 0;

	// cout stuff
	bool outputCurrentFrame = false;
	bool shouldOutput = false;

	// vars for metadata generation
	time_t gameStartTime;
	s32 lastFrame;
	std::unordered_map<u8, std::unordered_map<u8, u32>> characterUsage;

	void updateMetadataFields(u8 *payload, u32 length);
	void configureCommands(u8 *payload, u8 length);
	void writeToFileAsync(u8 *payload, u32 length, std::string fileOption);
	void writeToFile(std::unique_ptr<WriteMessage> msg);
	std::vector<u8> generateMetadata();
	void createNewFile();
	void closeFile();
	std::string generateFileName();
	bool checkFrameFullyFetched(s32 frameIndex);

	// std::ofstream log;

	File::IOFile m_file;
	std::vector<u8> m_payload;

	// online play stuff
	u16 getRandomStage();
	bool isDisconnected();
	bool isSlippiChatEnabled();
	void handleOnlineInputs(u8 *payload);
	void prepareOpponentInputs(s32 frame, bool shouldSkip);
	void handleSendInputs(s32 frame, u8 delay, s32 checksumFrame, u32 checksum, u8 *inputs);
	void handleCaptureSavestate(u8 *payload);
	void handleLoadSavestate(u8 *payload);
	void handleNameEntryLoad(u8 *payload);
	void startFindMatch(u8 *payload);
	void prepareOnlineMatchState();
	void setMatchSelections(u8 *payload);
	bool shouldSkipOnlineFrame(s32 frame, s32 finalizedFrame);
	void handlePoorMatchPerformance(s32 frame);
	bool shouldAdvanceOnlineFrame(s32 frame);
	bool opponentRunahead();
	void handleLogInRequest();
	void handleLogOutRequest();
	void handleUpdateAppRequest();
	void prepareOnlineStatus();
	void handleConnectionCleanup();
	void prepareNewSeed();
	void handleReportGame(const SlippiExiTypes::ReportGameQuery &query);
	void handleOverwriteSelections(const SlippiExiTypes::OverwriteSelectionsQuery &query);
	void handleGamePrepStepComplete(const SlippiExiTypes::GpCompleteStepQuery &query);
	void prepareGamePrepOppStep(const SlippiExiTypes::GpFetchStepQuery &query);
	void handleCompleteSet(const SlippiExiTypes::ReportSetCompletionQuery &query);
	void handleMatchStatusUpdate(const SlippiExiTypes::ReportMatchStatusUpdateQuery &query);
	void handleGetPlayerSettings();
	void handleGetRank();

	// replay playback stuff
	void prepareGameInfo(u8 *payload);
	void prepareGeckoList();
	void prepareCharacterFrameData(Slippi::FrameData *frame, u8 port, u8 isFollower);
	void prepareFrameData(u8 *payload);
	void prepareIsStockSteal(u8 *payload);
	void prepareIsFileReady();

	// misc stuff
	void handleChatMessage(u8 *payload);
	void logMessageFromGame(u8 *payload);
	void handleRoomCreate(u8 *payload);
	void handleRoomQueue(u8 *payload);
	void handleRoomJoin(u8 *payload);
	void handleRoomWatch();
	void handleRoomLeave(u8 *payload);
	void handleRoomStageDraft(u8 *payload);
	void prepareRoomDraftDrive();

	// True once a watch is up and has enough to show. Everything a watcher does
	// differently is gated on this, so a player's match takes exactly the paths
	// it took before.
	bool isWatching() const
	{
		if (!watch_client || !watch_client->Ready())
			return false;
		// ⚠ OVER counts. The players disconnect the moment THEIR game ends, and
		// a watcher that is behind still has a match to finish playing out of
		// the timeline it already holds. Dropping it here would cut the ending
		// off, which is the part worth watching.
		//
		// What ends a watched game is the game ending - Melee works that out
		// from the state it has simulated, exactly as it does for a player.
		SlippiWatchClient::Status st = watch_client->GetStatus();
		return st == SlippiWatchClient::Status::WATCHING || st == SlippiWatchClient::Status::OVER;
	}

	// Watching a match in this room. Null unless we are.
	std::unique_ptr<SlippiWatchClient> watch_client;
	void tellRoomsWhoWeAre();
	void prepareRoomState();
	void handleRoomList(u8 *payload);
	void prepareRoomList();
	void prepareFileLength(u8 *payload);
	void prepareFileLoad(u8 *payload);
	void prepareGctLength();
	void prepareGctLoad(u8 *payload);
	void prepareDelayResponse();
	void preparePremadeTextLength(u8 *payload);
	void preparePremadeTextLoad(u8 *payload);

	// helper functions
	bool doesTagMatchInput(u8 *input, u8 inputLen, std::string tag);

	std::vector<u8> loadPremadeText(u8 *payload);

	void FileWriteThread(void);

	Common::FifoQueue<std::unique_ptr<WriteMessage>, false> fileWriteQueue;
	bool writeThreadRunning = false;
	std::thread m_fileWriteThread;

	std::unordered_map<u8, std::string> getNetplayNames();

	std::vector<u8> playbackSavestatePayload;
	std::vector<u8> geckoList;

	u32 stallFrameCounts[SLIPPI_REMOTE_PLAYER_MAX] = {};
	u64 lastIntervalTimeUs = 0;
	s32 perfDebt = 0; // Leaky accumulator of poor-performance intervals (see handlePoorMatchPerformance)

	std::vector<u8> m_read_queue;
	std::unique_ptr<Slippi::SlippiGame> m_current_game = nullptr;
	SlippiSpectateServer *m_slippiserver = nullptr;
	SlippiMatchmaking::MatchSearchSettings lastSearch;
	SlippiMatchmaking::MatchmakeResult recentMmResult;

	// What WE last picked, remembered against ourselves rather than against a
	// port. The draft takes its default from the previous match's block, which is
	// indexed by port - and the port is decided fresh every pairing, so between
	// games a player was offered whatever the OTHER one had played.
	u8 my_last_char = 0;
	u8 my_last_color = 0;
	bool have_my_last = false;

	std::vector<u16> stagePool;

	// Used by ranked to set game prep selections
	std::vector<SlippiPlayerSelections> overwrite_selections;

	// Rooms: driving the draft's stage half when the room does not draft stages.
	//
	// The draft asks its two stage questions as steps 0 and 1 - a ban then a pick,
	// one player each - and only asks its own player. So they cannot be skipped
	// from here; they have to be ANSWERED, by moving the cursor and pressing A.
	//
	// ⚠ Every edge is a message, not a timer. The draft completes a step through
	// CMD_GP_COMPLETE_STEP and reads the other side's through CMD_GP_FETCH_STEP,
	// so Dolphin sees the whole negotiation and can say exactly when to start and
	// when to stop. See prepareRoomDraftDrive.
	int draft_last_local_step = -1;
	u8 draft_drive_hold = 0;
	u16 draft_drive_frame = 0;
	bool draft_drive_armed = false;
	u64 draft_drive_last_ask = 0;
	u16 draft_drive_listen = 0;
	int draft_fetch_step = -1;   // a step the draft asked us about, so NOT ours

	u32 frameSeqIdx = 0;

	bool isEnetInitialized = false;

	std::default_random_engine generator;

	// Frame skipping variables
	int framesToSkip = 0;
	bool isCurrentlySkipping = false;

	// Frame advancing variables
	int framesToAdvance = 0;
	bool isCurrentlyAdvancing = false;
	int fallBehindCounter = 0;
	int fallFarBehindCounter = 0;

	std::string forcedError = "";

	// Used to determine when to detect when a new session has started
	bool isPlaySessionActive = false;

	// We put these at the class level to preserve values in the case of a disconnect
	// while loading. Without this, someone could load into a game playing the wrong char
	u8 localPlayerIndex = 0;
	u8 remotePlayerIndex = 1;

  protected:
	void TransferByte(u8 &byte) override;

  private:
	SlippiPlayerSelections localSelections;

	std::unique_ptr<SlippiUser> user;
	std::unique_ptr<SlippiGameFileLoader> gameFileLoader;
	std::unique_ptr<SlippiNetplayClient> slippi_netplay;
	std::unique_ptr<SlippiMatchmaking> matchmaking;
	std::unique_ptr<SlippiDirectCodes> directCodes;
	std::unique_ptr<SlippiDirectCodes> teamsCodes;

	std::map<s32, std::unique_ptr<SlippiSavestate>> activeSavestates;
	std::deque<std::unique_ptr<SlippiSavestate>> availableSavestates;

	std::vector<u16> allowedStages;
};
