#pragma once

// Watching a room match, as a delay-based peer.
//
// The two people playing are connected to each other by Slippi's own servers
// and know nothing about us beyond an extra address to copy their inputs to.
// This end connects to BOTH of them, collects their pads into one timeline and
// hands Melee whole frames out of it.
//
// ⚠️ BOTH of them, not one. Each client sends only its OWN pads and its OWN
// selections - Send() never forwards what it received - so one connection is
// half a match.
//
// ⚠️ It never predicts, which is the whole point of doing it this way. Melee is
// only ever given a frame once both players' inputs for that frame are in hand,
// so there is nothing to roll back and nothing to fast-forward out of. When the
// network is late the picture stops; when it catches up the picture resumes. A
// watcher stalling costs the players nothing, because watchers send no acks and
// so take no part in trimming anyone's pad queue.

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "Common/CommonTypes.h"
#include "Core/Slippi/SlippiPad.h"

class SlippiWatchClient
{
  public:
	// Both players' addresses, and the local port to watch from.
	//
	// One socket for both of them on purpose: it is one hole through this end's
	// NAT, one address to publish, and one thing for both players to punch at.
	SlippiWatchClient(const std::vector<std::string> &addrs, const std::vector<u16> &ports, u16 localPort);
	~SlippiWatchClient();

	SlippiWatchClient(const SlippiWatchClient &) = delete;
	void operator=(const SlippiWatchClient &) = delete;

	enum class Status
	{
		CONNECTING,
		WATCHING,
		FAILED,
		OVER, // the players finished, or went away
	};

	Status GetStatus() const { return m_status.load(std::memory_order_acquire); }

	// The newest frame we hold for EVERY player, with no holes behind it. This
	// is how far Melee may safely be advanced, and it is deliberately not "the
	// newest thing we have heard" - a frame sitting past a hole is not usable.
	s32 LatestFrame() const { return m_contiguous.load(std::memory_order_acquire); }

	// One player's inputs for one frame. Returns false when we do not hold it,
	// in which case the caller must not advance.
	bool GetPad(s32 frame, u8 playerIdx, u8 *out) const;

	// What the two of them picked, as they told us. Empty until both have.
	struct Picks
	{
		bool known = false;
		u8 character[2] = {0, 0};
		u8 colour[2] = {0, 0};
		u16 stage = 0;
		u32 seed = 0;
	};
	Picks GetPicks() const;

	// Enough to start a match: we know what both of them picked, and we hold at
	// least the first frame of it.
	bool Ready() const;

	// ⚠ The watcher is port 2, not port 0.
	//
	// The old build stood in for port 0, which meant the two players arrived by
	// DIFFERENT routes - one through Melee's local input path, which holds a pad
	// back by the input delay, and one through the remote path, which does not.
	// Uncompensated that simulates a match neither of them played, and the
	// compensation is fiddly enough to be a bug of its own.
	//
	// Port 2 is not in the match - the block marks it empty - so both real
	// players come through the remote path identically, and the 1P port that
	// InitOnlinePlay reads points at a slot whose neutral controller moves
	// nothing.
	static const u8 WATCHER_PORT = 2;

  private:
	void ThreadFunc();
	void OnPacket(const u8 *data, size_t len, ENetPeer *from);
	void AskForMissing(s32 from);
	void Advance();

	ENetHost *m_host = nullptr;
	std::vector<ENetPeer *> m_players;
	std::thread m_thread;
	std::atomic<bool> m_run{true};
	std::atomic<Status> m_status{Status::CONNECTING};

	// ⚠️ Indexed by frame - Slippi::GAME_FIRST_FRAME, which is -123 and not 0.
	// Held per player, because they arrive from two different places and one can
	// be ahead of the other.
	struct Frame
	{
		std::array<u8, SLIPPI_PAD_DATA_SIZE> pad{};
		bool have = false;
	};

	mutable std::mutex m_lock;
	std::vector<Frame> m_line[2];
	std::atomic<s32> m_contiguous{0};
	s32 m_heard[2] = {0, 0}; // newest frame seen from each, holes and all
	Picks m_picks;
	bool m_toldPicks[2] = {false, false};

	u64 m_lastAskUs = 0;
};
