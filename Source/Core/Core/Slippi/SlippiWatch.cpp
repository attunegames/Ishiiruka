#include "Core/Slippi/SlippiWatch.h"

#include <algorithm>
#include <cstdlib>   // rand, for the STUN transaction id
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"
#include "Core/NetPlayProto.h"
#include "Core/Slippi/SlippiNetplay.h"
#include "Core/Slippi/SlippiRooms.h"
#include <SlippiLib/SlippiGame.h>

#include <SFML/Network/Packet.hpp>

namespace
{
// The timeline is addressed by offset from Melee's first frame, which is -123
// and not 0. Getting this wrong puts every lookup 123 frames out.
inline size_t SlotFor(s32 frame)
{
	return (size_t)(frame - Slippi::GAME_FIRST_FRAME);
}

// A match is a few minutes. Reserving the whole thing up front costs about
// 300KB for both players and means the timeline never moves under a reader.
const size_t kFramesReserved = 60 * 60 * 12; // twelve minutes
} // namespace

SlippiWatchClient::SlippiWatchClient(const std::vector<std::string> &addrs, const std::vector<u16> &ports,
                                     u16 localPort,
                                     const std::vector<std::pair<std::string, u16>> &fallback)
{
	m_fallback = fallback;
	m_needed = std::min<size_t>(addrs.size(), 2);

	for (int i = 0; i < 2; i++)
		m_line[i].resize(kFramesReserved);
	m_contiguous.store(Slippi::GAME_FIRST_FRAME - 1, std::memory_order_release);
	m_heard[0] = m_heard[1] = Slippi::GAME_FIRST_FRAME - 1;

// Ask a STUN server where the world sees THIS socket.
//
// ⚠ A watcher is the one client that cannot be told. Both players learn
// their public address for free - Slippi's matchmaking server reports where it
// saw each of them when it arranges the match, which is why nothing else here
// needs STUN. A watcher is in no match, gets no assignment, and so has no idea
// what its own address looks like from outside.
//
// It has to know, because the players cannot punch a hole towards an address
// nobody published. Without this the watcher's packets arrive at two routers
// that have never heard of it and are dropped - which is exactly what the
// first beta did, four times, "0 of 2 answered".
//
// ⚠ On the ENet host's OWN socket, before the service loop starts. A
// separate socket would be a separate NAT mapping and a different public
// port, so the address we published would not be the one the players' packets
// could reach. Once ThreadFunc is running, enet_host_service owns this socket
// and would swallow the reply as a malformed ENet packet.
static bool StunQuery(ENetSocket sock, const char *server, u16 serverPort, std::string &out)
{
	ENetAddress to;
	if (enet_address_set_host(&to, server) != 0)
		return false;
	to.port = serverPort;

	// A binding request: type 0x0001, no attributes, the magic cookie, and a
	// transaction id we can recognise the answer by.
	u8 req[20] = {0};
	req[1] = 0x01;
	req[4] = 0x21; req[5] = 0x12; req[6] = 0xA4; req[7] = 0x42;
	for (int i = 8; i < 20; i++)
		req[i] = (u8)(rand() & 0xFF);

	ENetBuffer buf;
	buf.data = req;
	buf.dataLength = sizeof(req);
	if (enet_socket_send(sock, &to, &buf, 1) <= 0)
		return false;

	// Three short waits rather than one long one: the first packet out of a
	// fresh socket is the one most likely to be lost.
	for (int attempt = 0; attempt < 3; attempt++)
	{
		enet_uint32 cond = ENET_SOCKET_WAIT_RECEIVE;
		if (enet_socket_wait(sock, &cond, 300) != 0 || !(cond & ENET_SOCKET_WAIT_RECEIVE))
		{
			enet_socket_send(sock, &to, &buf, 1);
			continue;
		}

		u8 resp[512];
		ENetAddress from;
		ENetBuffer rbuf;
		rbuf.data = resp;
		rbuf.dataLength = sizeof(resp);
		int got = enet_socket_receive(sock, &from, &rbuf, 1);
		if (got < 20)
			continue;

		// A binding SUCCESS response carrying our transaction id.
		if (resp[0] != 0x01 || resp[1] != 0x01 || memcmp(resp + 8, req + 8, 12) != 0)
			continue;

		int len = (resp[2] << 8) | resp[3];
		int at = 20;
		while (at + 4 <= 20 + len && at + 4 <= got)
		{
			int type = (resp[at] << 8) | resp[at + 1];
			int alen = (resp[at + 2] << 8) | resp[at + 3];
			const u8 *val = resp + at + 4;

			// XOR-MAPPED-ADDRESS, IPv4. The port and address are XORed with the
			// cookie, which is what stops a naive middlebox rewriting them.
			if (type == 0x0020 && alen >= 8 && val[1] == 0x01)
			{
				u16 port = (u16)(((val[2] << 8) | val[3]) ^ 0x2112);
				u8 ip[4];
				ip[0] = val[4] ^ 0x21;
				ip[1] = val[5] ^ 0x12;
				ip[2] = val[6] ^ 0xA4;
				ip[3] = val[7] ^ 0x42;
				out = StringFromFormat("%d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], port);
				return true;
			}
			at += 4 + ((alen + 3) & ~3);
		}
	}
	return false;
}

	if (enet_initialize() != 0)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Watch] could not start ENet");
		m_status.store(Status::FAILED, std::memory_order_release);
		return;
	}

	// Bound to a known local port, like the netplay client is, because the
	// players are punching at the address a STUN reply gave for THIS socket and
	// a different port would not get through.
	ENetAddress local;
	local.host = ENET_HOST_ANY;
	local.port = localPort;

	m_host = enet_host_create(&local, 4, 3, 0, 0);
	if (!m_host)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Watch] could not open a socket on port %d", localPort);
		m_status.store(Status::FAILED, std::memory_order_release);
		return;
	}

	for (size_t i = 0; i < addrs.size() && i < 2; i++)
	{
		ENetAddress addr;
		if (enet_address_set_host(&addr, addrs[i].c_str()) != 0)
		{
			ERROR_LOG(SLIPPI_ONLINE, "[Watch] could not read the address %s", addrs[i].c_str());
			continue;
		}
		addr.port = ports[i];

		// ⚠️ The last argument is what marks us as a watcher rather than a
		// player. Without it the other end files an unrecognised peer as remote
		// player 0 and sets that player's active flag from it.
		ENetPeer *peer = enet_host_connect(m_host, &addr, 3, SLIPPI_CONNECT_SPECTATOR);
		if (!peer)
		{
			ERROR_LOG(SLIPPI_ONLINE, "[Watch] no peer slot for %s", addrs[i].c_str());
			continue;
		}
		m_players.push_back(peer);
		m_peerSlot.emplace_back(peer, (u8)i);
		WARN_LOG(SLIPPI_ONLINE, "[Watch] asking to watch %s:%d", addrs[i].c_str(), ports[i]);
	}

	if (m_players.empty())
	{
		m_status.store(Status::FAILED, std::memory_order_release);
		return;
	}

	m_dialledAtUs = Common::Timer::GetTimeUs();
	m_thread = std::thread(&SlippiWatchClient::ThreadFunc, this);
}

SlippiWatchClient::~SlippiWatchClient()
{
	m_run.store(false, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();

	if (m_host)
	{
		for (auto *peer : m_players)
		{
			if (peer)
				enet_peer_disconnect(peer, 0);
		}

		// Give the goodbyes a moment to go out, then stop caring.
		ENetEvent ev;
		while (enet_host_service(m_host, &ev, 250) > 0)
		{
			if (ev.type == ENET_EVENT_TYPE_RECEIVE)
				enet_packet_destroy(ev.packet);
		}

		for (auto *peer : m_players)
		{
			if (peer)
				enet_peer_reset(peer);
		}
		enet_host_destroy(m_host);
		m_host = nullptr;
	}
}

void SlippiWatchClient::ThreadFunc()
{
	Common::SetCurrentThreadName("Slippi watch");

	// Where the world sees this socket, so the players can punch towards it.
	//
	// ⚠ ON THIS THREAD, not in the constructor. StunQuery waits for a reply
	// and retries twice, so it can take the best part of a second - and the
	// constructor runs on the GAME thread, where that is a visible freeze the
	// moment somebody presses Y.
	//
	// Publishing from here is also the right order. The dial has already gone out
	// by now and ENet keeps retrying it, so the punch only has to land inside the
	// twenty seconds this end waits before giving up - which it does, because a
	// tick is a second at most.
	//
	// Failure is not fatal: on a local network there is no NAT to open and
	// watching works without any of this.
	if (StunQuery(m_host->socket, "stun.l.google.com", 19302, m_publicAddr))
	{
		WARN_LOG(SLIPPI_ONLINE, "[Watch] this end is %s from outside", m_publicAddr.c_str());
		Rooms::SetWatchAddress(m_publicAddr);
	}
	else
	{
		WARN_LOG(SLIPPI_ONLINE, "[Watch] STUN did not answer - the players cannot be "
		                        "told where to punch, so this will only work on a LAN");
	}

	size_t connected = 0;
	u64 lastWaitLogUs = 0;

	// The peers that actually answered. ⚠️ A dial that never answers ALSO ends in
	// a disconnect event - ENet gives up on it after 30 seconds - and that must
	// not be mistaken for a player leaving. See the disconnect case below.
	std::vector<ENetPeer *> live;

	while (m_run.load(std::memory_order_acquire))
	{
		ENetEvent ev;
		int got = enet_host_service(m_host, &ev, 100);
		if (got < 0)
		{
			ERROR_LOG(SLIPPI_ONLINE, "[Watch] the socket failed");
			m_status.store(Status::FAILED, std::memory_order_release);
			return;
		}

		if (got > 0)
		{
			switch (ev.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				connected++;
				live.push_back(ev.peer);
				WARN_LOG(SLIPPI_ONLINE, "[Watch] connected to %d of %d", (int)connected, (int)m_needed);

				// Ask for everything from the start. On a match we joined at the
				// beginning this asks for nothing and costs one packet; on one
				// already under way it is the whole thing so far.
				sf::Packet ask;
				ask << static_cast<MessageId>(NP_MSG_SLIPPI_WATCH_FROM);
				ask << (s32)Slippi::GAME_FIRST_FRAME;
				ENetPacket *epac = enet_packet_create(ask.getData(), ask.getDataSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(ev.peer, 0, epac);

				// ⚠ m_needed, not m_players.size(): the test fallback adds more
				// peers aimed at the same two people, and waiting for all of them
				// would mean waiting for a duplicate of a connection we have.
				if (connected >= m_needed)
					m_status.store(Status::WATCHING, std::memory_order_release);
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
				OnPacket(ev.packet->data, ev.packet->dataLength, ev.peer);
				enet_packet_destroy(ev.packet);
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				// ⚠️ A peer we never reached, giving up. ENet reports a dial that
				// timed out through this same event, 30 seconds after it was made,
				// and it looks exactly like a player leaving. On a test rig that is
				// guaranteed to happen: the two hairpin addresses are dialled and
				// never answer, so every watch used to die half a minute in no
				// matter how well it was going.
				if (std::find(live.begin(), live.end(), ev.peer) == live.end())
				{
					WARN_LOG(SLIPPI_ONLINE, "[Watch] an address that never answered has given up - still watching");
					m_players.erase(std::remove(m_players.begin(), m_players.end(), ev.peer), m_players.end());
					break;
				}

				// One of them going is the end of it. We hold half a match and
				// half a match cannot be simulated - better to say so than to
				// show a game that is not happening.
				WARN_LOG(SLIPPI_ONLINE, "[Watch] a player went away - that is the end of the view");
				m_status.store(Status::OVER, std::memory_order_release);
				return;

			default:
				break;
			}
		}

		// ⚠⚠ TEST RIGS ONLY - DELETE BEFORE THE FIRST BETA ⚠⚠
		//
		// The real address has had its chance and nobody answered. On a rig where
		// every client is behind one router that is the expected outcome: the
		// players' public address is OUR public address, and a packet sent to it
		// is a hairpin that most routers drop.
		//
		// Tried SECOND, never first, so a watcher out on the internet reaches the
		// real address and never spends a moment on this. And the addresses only
		// exist at all when the players' build had lanForTesting on.
		if (!m_fallback.empty() && connected < m_needed &&
		    Common::Timer::GetTimeUs() - m_dialledAtUs > 3000000)
		{
			u8 fallbackSlot = 0;
			for (const auto &f : m_fallback)
			{
				ENetAddress addr;
				if (enet_address_set_host(&addr, f.first.c_str()) != 0)
					continue;
				addr.port = f.second;
				ENetPeer *peer = enet_host_connect(m_host, &addr, 3, SLIPPI_CONNECT_SPECTATOR);
				if (peer)
				{
					m_players.push_back(peer);
					m_peerSlot.emplace_back(peer, fallbackSlot);
					WARN_LOG(SLIPPI_ONLINE, "[Watch] ⚠ nobody answered - trying the test address %s:%d",
					         f.first.c_str(), f.second);
				}
				fallbackSlot++;
			}
			m_fallback.clear();
		}

		// Say something while nothing is happening.
		//
		// ⚠️ A watch that never connects used to be completely silent: the status
		// stayed CONNECTING for ever, Melee stayed where it was, and from the
		// outside pressing Y "did nothing". A watcher whose players are
		// unreachable is a normal outcome - a restrictive NAT on either end does
		// it - so it has to report itself rather than hang.
		if (m_status.load(std::memory_order_acquire) == Status::CONNECTING)
		{
			const u64 waited = Common::Timer::GetTimeUs() - m_dialledAtUs;
			if (waited > 20000000) // twenty seconds
			{
				ERROR_LOG(SLIPPI_ONLINE, "[Watch] gave up - reached %d of %d players in 20s", (int)connected,
				          (int)m_needed);
				m_status.store(Status::FAILED, std::memory_order_release);
				return;
			}
			if (waited - lastWaitLogUs > 5000000) // every five seconds
			{
				lastWaitLogUs = waited;
				WARN_LOG(SLIPPI_ONLINE, "[Watch] still waiting - %d of %d answered after %ds", (int)connected,
				         (int)m_needed, (int)(waited / 1000000));
			}
		}

		// Nothing to ask for until we are actually behind.
		if (m_status.load(std::memory_order_acquire) == Status::WATCHING)
		{
			s32 behind;
			{
				std::lock_guard<std::mutex> lk(m_lock);
				behind = std::max(m_heard[0], m_heard[1]) - m_contiguous.load(std::memory_order_acquire);
			}
			// A couple of frames behind is just the network. A second behind
			// with newer frames already in hand is a hole that will not fill
			// itself, because nothing resends the live stream.
			if (behind > 60)
				AskForMissing(m_contiguous.load(std::memory_order_acquire) + 1);
		}
	}
}

// "I hold everything up to N, send me what follows."
//
// ⚠️ Rate limited. The answer is a burst of reliable packets and it takes a
// moment to arrive; asking again every trip round the loop would have the
// players resending the same stretch of the match over and over.
void SlippiWatchClient::AskForMissing(s32 from)
{
	u64 now = Common::Timer::GetTimeUs();
	if (now - m_lastAskUs < 500000) // half a second
		return;
	m_lastAskUs = now;

	sf::Packet ask;
	ask << static_cast<MessageId>(NP_MSG_SLIPPI_WATCH_FROM);
	ask << from;

	for (auto *peer : m_players)
	{
		if (!peer)
			continue;
		ENetPacket *epac = enet_packet_create(ask.getData(), ask.getDataSize(), ENET_PACKET_FLAG_RELIABLE);
		enet_peer_send(peer, 0, epac);
	}
	WARN_LOG(SLIPPI_ONLINE, "[Watch] asking both of them for frame %d onwards", from);
}

void SlippiWatchClient::OnPacket(const u8 *data, size_t len, ENetPeer *from)
{
	if (len < 1)
		return;

	sf::Packet packet;
	packet.append(data, len);

	MessageId mid = 0;
	if (!(packet >> mid))
		return;

	switch (mid)
	{
	case NP_MSG_SLIPPI_MATCH_SELECTIONS:
	{
		// Same order writeToPacket puts them in. Read in full even though only
		// some of it is used, because a short read leaves the stream misaligned.
		u8 characterId = 0, characterColor = 0, playerIdx = 0, teamId = 0, altStage = 0;
		bool isCharacterSelected = false, isStageSelected = false;
		u16 stageId = 0;
		u32 rngOffset = 0;

		if (!(packet >> characterId >> characterColor >> isCharacterSelected))
			return;
		if (!(packet >> playerIdx))
			return;
		if (!(packet >> stageId >> isStageSelected))
			return;
		if (!(packet >> rngOffset))
			return;
		if (!(packet >> teamId))
			return;
		if (!(packet >> altStage))
			return;

		if (playerIdx > 1)
			return;

		std::lock_guard<std::mutex> lk(m_lock);
		if (isCharacterSelected)
		{
			m_picks.character[playerIdx] = characterId;
			m_picks.colour[playerIdx] = characterColor;
			m_toldPicks[playerIdx] = true;

			// Which address this one answered on, so a name can be put to them.
			for (const auto &ps : m_peerSlot)
			{
				if (ps.first == from)
				{
					m_picks.slot[playerIdx] = ps.second;
					break;
				}
			}
		}
		// ⚠️ Resolved the way the players resolve it, which is "the first of them
		// in port order who chose one" - EXI_DeviceSlippi walks orderedSelections
		// and breaks on the first isStageSelected. Taking whichever arrived last
		// would put the watcher on a different stage from the match.
		if (isStageSelected)
		{
			m_stageOf[playerIdx] = stageId;
			m_stageSet[playerIdx] = true;
			m_picks.stage = m_stageSet[0] ? m_stageOf[0] : m_stageOf[1];
		}
		// ⚠️ PLAYER 0's seed, and nobody else's. Both of them generate their own
		// rngOffset, but the match runs on the decider's, and the decider is
		// player index 0 - EXI_DeviceSlippi does "rngOffset = isDecider ?
		// lps.rngOffset : rps[0].rngOffset", which is player 0 either way. Taking
		// whichever arrived last is a coin flip on the seed, and a wrong seed is
		// every random thing in the match happening differently.
		if (playerIdx == 0 && rngOffset)
			m_picks.seed = rngOffset;
		m_picks.known = m_toldPicks[0] && m_toldPicks[1];

		WARN_LOG(SLIPPI_ONLINE, "[Watch] player %d is %d (colour %d), stage %d, seed %08x%s", playerIdx,
		         characterId, characterColor, stageId, rngOffset,
		         isCharacterSelected ? "" : " - NOT CHOSEN YET");
		break;
	}
	case NP_MSG_SLIPPI_PAD:
	{
		// ⚠️ The frame in the header is the NEWEST in the packet and the pads run
		// BACKWARDS from it - index i is frame-i. Reading it forwards plays the
		// match in reverse.
		s32 newest;
		u8 playerIdx;
		s32 checksumFrame;
		u32 checksum;
		if (!(packet >> newest))
			return;
		if (!(packet >> playerIdx))
			return;
		if (!(packet >> checksumFrame))
			return;
		if (!(packet >> checksum))
			return;
		if (playerIdx > 1)
			return;

		const size_t kHeader = 14; // mid + frame + idx + checksumFrame + checksum
		if (len <= kHeader)
			return;
		const size_t count = (len - kHeader) / SLIPPI_PAD_DATA_SIZE;

		std::lock_guard<std::mutex> lk(m_lock);
		for (size_t i = 0; i < count; i++)
		{
			s32 frame = newest - (s32)i;
			if (frame < Slippi::GAME_FIRST_FRAME)
				break;
			size_t slot = SlotFor(frame);
			if (slot >= m_line[playerIdx].size())
				continue; // past the end of a very long match

			// First writer wins. A frame that arrives twice - once live, once in
			// a catch-up burst - is the same frame, and rewriting it would be
			// changing history under a reader.
			if (m_line[playerIdx][slot].have)
				continue;
			memcpy(m_line[playerIdx][slot].pad.data(), data + kHeader + i * SLIPPI_PAD_DATA_SIZE,
			       SLIPPI_PAD_DATA_SIZE);
			m_line[playerIdx][slot].have = true;
		}

		if (newest > m_heard[playerIdx])
			m_heard[playerIdx] = newest;

		Advance();
		break;
	}
	default:
		break;
	}

	(void)from;
}

// How far the picture may safely go: the newest frame we hold for BOTH of them
// with nothing missing behind it.
//
// ⚠️ Called with m_lock held.
void SlippiWatchClient::Advance()
{
	const s32 ceiling = std::min(m_heard[0], m_heard[1]);

	// ⚠ Where the timeline STARTS is found, not assumed.
	//
	// It began at Slippi::GAME_FIRST_FRAME - 1, which says the first frame we
	// want is -123. If the players' history does not reach back that far - they
	// only keep what they have sent, and a match already under way has been
	// trimmed - then the very first check fails and this never advances a single
	// frame. Silently, and for the rest of the match.
	//
	// So the first time both of them have said anything, walk forward to the
	// earliest frame they BOTH hold and start from there.
	if (!m_baselined && m_heard[0] > Slippi::GAME_FIRST_FRAME - 1 && m_heard[1] > Slippi::GAME_FIRST_FRAME - 1)
	{
		for (s32 f = Slippi::GAME_FIRST_FRAME; f <= ceiling; f++)
		{
			size_t slot = SlotFor(f);
			if (slot >= m_line[0].size())
				break;
			if (m_line[0][slot].have && m_line[1][slot].have)
			{
				m_contiguous.store(f - 1, std::memory_order_release);
				m_baselined = true;
				WARN_LOG(SLIPPI_ONLINE, "[Watch] the timeline starts at frame %d", f);
				break;
			}
		}
		if (!m_baselined)
			return; // nothing either of them holds in common yet
	}

	s32 at = m_contiguous.load(std::memory_order_relaxed);

	while (at < ceiling)
	{
		size_t slot = SlotFor(at + 1);
		if (slot >= m_line[0].size())
			break;
		if (!m_line[0][slot].have || !m_line[1][slot].have)
			break;
		at++;
	}

	m_contiguous.store(at, std::memory_order_release);
}

bool SlippiWatchClient::GetPad(s32 frame, u8 playerIdx, u8 *out) const
{
	if (playerIdx > 1 || frame < Slippi::GAME_FIRST_FRAME)
		return false;

	std::lock_guard<std::mutex> lk(m_lock);
	size_t slot = SlotFor(frame);
	if (slot >= m_line[playerIdx].size() || !m_line[playerIdx][slot].have)
		return false;

	memcpy(out, m_line[playerIdx][slot].pad.data(), SLIPPI_PAD_DATA_SIZE);
	return true;
}

bool SlippiWatchClient::Ready() const
{
	std::lock_guard<std::mutex> lk(m_lock);
	// Baselined means we have found where the timeline starts and hold at least
	// one frame of it from both of them. Comparing against GAME_FIRST_FRAME
	// instead would never come true for a match already under way.
	return m_picks.known && m_baselined;
}

SlippiWatchClient::Picks SlippiWatchClient::GetPicks() const
{
	std::lock_guard<std::mutex> lk(m_lock);
	return m_picks;
}
