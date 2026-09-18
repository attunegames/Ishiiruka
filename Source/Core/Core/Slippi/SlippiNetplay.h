// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/FifoQueue.h"
#include "Common/Timer.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "Core/Slippi/SlippiPad.h"
#include "InputCommon/GCPadStatus.h"
#include <SFML/Network/Packet.hpp>
#include <SlippiLib/SlippiGame.h>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <Qos2.h>
#endif

#define ROLLBACK_MAX_FRAMES 7
#define SLIPPI_ONLINE_LOCKSTEP_INTERVAL 30 // Number of frames to wait before attempting to time-sync
#define SLIPPI_PING_DISPLAY_INTERVAL 60
#define SLIPPI_REMOTE_PLAYER_MAX 3

// What a watcher puts in the ENet connect packet's user data so the people
// playing can tell it apart from a player.
//
// ⚠ This is not decoration. The connect handler used to take ANY unrecognised
// peer and file it as remote player 0 - earlyConnRemoteIdx defaults to 0 and
// nothing rejected an address that was not in the match - which set that
// player's "active" flag from a stranger's connection. A watcher has to say
// what it is on the way in, and anything that does not say stays a player, so
// the path the two people playing take is unchanged.
//
// Players connect with 0, which is what enet_host_connect has always been
// passed here.
#define SLIPPI_CONNECT_SPECTATOR 0x50535043 // 'PSPC'
#define SLIPPI_REMOTE_PLAYER_COUNT 3
#define SLIPPI_PLAYER_COUNT_MAX (SLIPPI_REMOTE_PLAYER_MAX + 1)

struct SlippiRemotePadOutput
{
	s32 latestFrame;
	s32 checksumFrame;
	u32 checksum;
	u8 playerIdx;
	bool isDisconnected = false;
	std::vector<u8> data;
};

struct SlippiGamePrepStepResults
{
	u8 step_idx;
	u8 char_selection;
	u8 char_color_selection;
	u8 stage_selections[2];
};

struct SlippiSyncedFighterState
{
	u8 stocks_remaining = 4;
	u16 current_health = 0;
};

struct SlippiSyncedGameState
{
	std::string match_id = "";
	u32 game_index = 0;
	u32 tiebreak_index = 0;
	u32 seconds_remaining = 480;
	SlippiSyncedFighterState fighters[4];
};

struct SlippiDesyncRecoveryResp
{
	bool is_recovering = false;
	bool is_waiting = false;
	bool is_error = false;
	SlippiSyncedGameState state;
};

class SlippiPlayerSelections
{
  public:
	u8 playerIdx = 0;
	u8 characterId = 0;
	u8 characterColor = 0;
	u8 teamId = 0;

	bool isCharacterSelected = false;

	u16 stageId = 0;
	bool isStageSelected = false;
	u8 alt_stage_mode{};

	u32 rngOffset = 0;

	int messageId = 0;
	bool error = false;

	void Merge(SlippiPlayerSelections &s)
	{
		this->rngOffset = s.rngOffset;

		if (s.isStageSelected)
		{
			this->stageId = s.stageId;
			this->isStageSelected = true;
			this->alt_stage_mode = s.alt_stage_mode;
		}

		if (s.isCharacterSelected)
		{
			this->characterId = s.characterId;
			this->characterColor = s.characterColor;
			this->teamId = s.teamId;
			this->isCharacterSelected = true;
		}
	}

	void Reset()
	{
		characterId = 0;
		characterColor = 0;
		isCharacterSelected = false;
		teamId = 0;

		stageId = 0;
		isStageSelected = false;

		rngOffset = 0;
	}
};

struct ChecksumEntry
{
	s32 frame;
	u32 value;
};

class SlippiMatchInfo
{
  public:
	SlippiPlayerSelections localPlayerSelections;
	SlippiPlayerSelections remotePlayerSelections[SLIPPI_REMOTE_PLAYER_MAX];

	void Reset()
	{
		localPlayerSelections.Reset();
		for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++)
		{
			remotePlayerSelections[i].Reset();
		}
	}
};

class OnlinePlayMode;
class SlippiNetplayClient
{
  public:
	void ThreadFunc();
	void SendAsync(std::unique_ptr<sf::Packet> packet);

	SlippiNetplayClient(bool isDecider); // Make a dummy client

	// A watcher's stand-in. No peers, no thread, no connection - it exists
	// because the match-building code reads what everyone picked out of a
	// netplay client, and a watcher was told all of that over its own
	// connections instead of negotiating it.
	//
	// `playerIdx` is the watcher's own port, which is NOT one of the two
	// playing. See SlippiWatchClient::WATCHER_PORT.
	void MakeWatcher(u8 playerIdx);
	void SetRemoteSelections(u8 remoteIdx, const SlippiPlayerSelections &s);
	SlippiNetplayClient(std::vector<std::string> addrs, std::vector<u16> ports, const u8 remotePlayerCount,
	                    const u16 localPort, bool isDecider, u8 playerIdx);
	~SlippiNetplayClient();

	// Slippi Online
	enum class SlippiConnectStatus
	{
		NET_CONNECT_STATUS_UNSET,
		NET_CONNECT_STATUS_INITIATED,
		NET_CONNECT_STATUS_CONNECTED,
		NET_CONNECT_STATUS_FAILED,
		NET_CONNECT_STATUS_DISCONNECTED,
	};

	// Reason carried over the wire alongside an intentional disconnect, via the ENet
	// disconnect data field. Values are transmitted as u32; 0 (UNSPECIFIED) is what a
	// normal/organic disconnect sends, so any non-zero value is a deliberate reason.
	enum class SlippiDisconnectReason : u32
	{
		UNSPECIFIED = 0,
		POOR_PERFORMANCE = 1,
	};

	bool IsDecider();
	bool IsConnectionSelected();
	u8 LocalPlayerPort();
	SlippiConnectStatus GetSlippiConnectStatus();
	std::vector<int> GetFailedConnections();
	void StartSlippiGame();
	void SendConnectionSelected();
	void SendSlippiPad(std::unique_ptr<SlippiPad> pad);
	void SetMatchSelections(SlippiPlayerSelections &s);
	void SendGamePrepStep(SlippiGamePrepStepResults &s);
	void SendSyncedGameState(SlippiSyncedGameState &s);
	bool GetGamePrepResults(u8 stepIdx, SlippiGamePrepStepResults &res);
	std::unique_ptr<SlippiRemotePadOutput> GetFakePadOutput(int frame);
	std::unique_ptr<SlippiRemotePadOutput> GetSlippiRemotePad(int index, int maxFrameCount);
	void DropOldRemoteInputs(int32_t finalizedFrame);
	std::unordered_map<u8, bool> GetActivePlayerIndices();
	void ForceDisconnectPlayer(u8 playerIdx);
	void ForceDisconnect(SlippiDisconnectReason reason = SlippiDisconnectReason::UNSPECIFIED);
	SlippiDisconnectReason GetDisconnectReason();
	SlippiMatchInfo *GetMatchInfo();
	SlippiPlayerSelections GetSlippiRemoteChatMessage(bool isChatEnabled);
	u8 GetSlippiRemoteSentChatMessage(bool isChatEnabled);
	s32 CalcTimeOffsetUs();
	double GetAndResetAvgPingMs();
	bool IsWaitingForDesyncRecovery();
	SlippiDesyncRecoveryResp GetDesyncRecoveryState();

	void WriteChatMessageToPacket(sf::Packet &packet, int messageId, u8 playerIdx);
	std::unique_ptr<SlippiPlayerSelections> ReadChatMessageFromPacket(sf::Packet &packet);

	std::unique_ptr<SlippiPlayerSelections> remoteChatMessageSelection =
	    nullptr;                    // most recent chat message player selection (message + player index)
	u8 remoteSentChatMessageId = 0; // most recent chat message id that current player sent

  protected:
	struct
	{
		std::recursive_mutex game;
		// lock order
		std::recursive_mutex players;
		std::recursive_mutex async_queue_write;
	} m_crit;

	Common::FifoQueue<std::unique_ptr<sf::Packet>, false> m_async_queue;

	ENetHost *m_client = nullptr;
	std::vector<ENetPeer *> m_server;

	// People watching, who are NOT players.
	//
	// Kept apart from m_server on purpose. m_server is what the match is made
	// of - it decides when everyone is connected, it is what disconnects end the
	// game, and its indices are player indices. A watcher is none of those
	// things: they arrive whenever, they leave whenever, and neither should be
	// felt by the two people playing.
	//
	// The only thing they share is the pad stream, which Send() copies to them.
	//
	// ⚠ Owned by the network thread, like m_server and activeConnections.
	std::vector<ENetPeer *> m_spectators;

	// ⚠ Six. Every watcher is another small UDP send per frame from a machine
	// that is mid-match, and the ENet host is built with ten peer slots of which
	// the players hold some - a room can hold far more people than that, so
	// without a ceiling a full room piles onto two of them.
	static const size_t MAX_SPECTATORS = 6;

	// Our own pads, kept for the whole match, for watchers only.
	//
	// ⚠ Nothing else keeps them. localPadQueue is trimmed the moment the other
	// player acks, and the pad stream is unreliable and carries only the few
	// un-acked frames as redundancy - so a watcher that loses a burst longer
	// than that has lost those frames for good unless somebody kept them. This
	// is that somebody. A whole match is about 15k frames of 8 bytes, so the
	// cost of never trimming it is well under a megabyte.
	std::vector<std::array<u8, SLIPPI_PAD_DATA_SIZE>> m_watchHistory;
	s32 m_watchHistoryFirstFrame = 0;

	void SendWatchHistoryFrom(ENetPeer *peer, s32 fromFrame);

	// What we picked, as it was when THIS game started.
	//
	// ⚠️ StartSlippiGame ends with matchInfo.Reset(), ready for the next game, so
	// by the time a match is on screen our own selections are all zeros. A
	// watcher arriving mid-match was being told "character 0, colour 0, stage 0"
	// and, having no idea what it was looking at, never started anything.
	SlippiPlayerSelections m_watchSelections;

	std::thread m_thread;
	u8 m_remotePlayerCount = 0;

	std::string m_selected_game;
	Common::Flag m_is_running{false};
	Common::Flag m_do_loop{true};

	unsigned int m_minimum_buffer_size = 6;

	u32 m_current_game = 0;

	// Slippi Stuff
	struct FrameTiming
	{
		int32_t frame;
		u64 timeUs;
	};

	struct FrameOffsetData
	{
		// TODO: Should the buffer size be dynamic based on time sync interval or not?
		int idx;
		std::vector<s32> buf;
	};

	bool isConnectionSelected = false;
	bool isDecider = false;
	bool hasGameStarted = false;
	u8 playerIdx = 0;

	struct ActiveConnectionInfo
	{
		u8 playerIdx;
		bool isDisconnected = false;
	};

	// Owned by the network thread (constructor + ThreadFunc + Send/OnData which run on the
	// network thread via the SendAsync queue). Do not read from other threads — use the
	// playerActive atomics below for cross-thread checks of liveness.
	std::unordered_map<std::string, std::map<ENetPeer *, ActiveConnectionInfo>> activeConnections;

	// Lock-free view of which global player indices still have at least one live peer.
	// Written by the network thread when activeConnections changes, and by the EXI
	// thread via ForceDisconnectPlayer. Read from any thread (notably the main/EXI
	// thread via GetActivePlayerIndices). The network thread also uses this to drive
	// per-peer ENet disconnects for players force-dropped from the EXI side.
	std::atomic<bool> playerActive[SLIPPI_PLAYER_COUNT_MAX] = {};

	std::deque<std::unique_ptr<SlippiPad>> localPadQueue; // most recent inputs at start of deque
	std::deque<std::unique_ptr<SlippiPad>>
	    remotePadQueue[SLIPPI_REMOTE_PLAYER_MAX]; // most recent inputs at start of deque

	bool is_desync_recovery = false;
	ChecksumEntry remote_checksums[SLIPPI_REMOTE_PLAYER_MAX];
	SlippiSyncedGameState remote_sync_states[SLIPPI_REMOTE_PLAYER_MAX];
	SlippiSyncedGameState local_sync_state;

	std::deque<SlippiGamePrepStepResults> gamePrepStepQueue;

	u64 pingUs[SLIPPI_REMOTE_PLAYER_MAX];

	// Ping accumulator for the poor-performance check. The network thread adds every ack-derived
	// ping measurement (all remote players pooled together), and the EXI/CPU thread drains it once
	// per performance interval via GetAndResetAvgPingMs(). This is the only ping data meant to be
	// read off the network thread, which is why these are atomic while pingUs above is not.
	std::atomic<u64> pingSampleSumUs{0};
	std::atomic<u64> pingSampleCount{0};
	int32_t lastFrameAcked[SLIPPI_REMOTE_PLAYER_MAX];
	FrameOffsetData frameOffsetData[SLIPPI_REMOTE_PLAYER_MAX];
	FrameTiming lastFrameTiming[SLIPPI_REMOTE_PLAYER_MAX];
	std::array<Common::FifoQueue<FrameTiming, false>, SLIPPI_REMOTE_PLAYER_MAX> ackTimers;

	std::atomic<SlippiConnectStatus> slippiConnectStatus{SlippiConnectStatus::NET_CONNECT_STATUS_UNSET};

	// Disconnect reason plumbing (see SlippiDisconnectReason). m_pendingDisconnectReason is set by the
	// EXI thread before flipping playerActive and is read by the network thread when it issues
	// enet_peer_disconnect so the peer learns why. m_disconnectReason is the resolved reason for this
	// client — set locally on the initiating side, or from the received disconnect data on the receiving
	// side — and is read by the EXI thread to drive UI such as the poor-performance OSD.
	std::atomic<u32> m_pendingDisconnectReason{static_cast<u32>(SlippiDisconnectReason::UNSPECIFIED)};
	std::atomic<u32> m_disconnectReason{static_cast<u32>(SlippiDisconnectReason::UNSPECIFIED)};

	std::vector<int> failedConnections;
	SlippiMatchInfo matchInfo;

	bool m_is_recording = false;

	void writeToPacket(sf::Packet &packet, SlippiPlayerSelections &s);
	std::unique_ptr<SlippiPlayerSelections> readSelectionsFromPacket(sf::Packet &packet);

  private:
	u8 PlayerIdxFromPort(u8 port);
	unsigned int OnData(sf::Packet &packet, ENetPeer *peer);
	void Send(sf::Packet &packet);
	void SendToSpectators(sf::Packet &packet);
	void Disconnect();
	// Network-thread only — call from inside ThreadFunc.
	bool AreAllConnectionsDisconnected();
	bool AreAllPeersDisconnectedForKey(const std::string &key);

	bool m_is_connected = false;

#ifdef _WIN32
	HANDLE m_qos_handle;
	QOS_FLOWID m_qos_flow_id;
#endif

	u32 m_timebase_frame = 0;
};
extern SlippiNetplayClient *SLIPPI_NETPLAY; // singleton static pointer

static bool IsOnline()
{
	return SLIPPI_NETPLAY != nullptr;
}
