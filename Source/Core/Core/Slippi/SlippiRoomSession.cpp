#include "SlippiRoomSession.h"

#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#ifdef USE_UPNP
#include <miniupnpc.h>
#include <upnpcommands.h>
#endif

// #define LOCAL_TESTING

using json = nlohmann::json;

namespace
{
const int CONNECT_TIMEOUT_MS = 5000;
const int SERVICE_INTERVAL_MS = 20;

#ifdef USE_UPNP
// Opens the room's port on the router so members can connect to the host. Rooms still work
// without it when the port is forwarded some other way
bool mapPort(u16 port, UPNPUrls &urls, IGDdatas &data)
{
	int error = 0;
	UPNPDev *devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
	if (!devlist)
	{
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] No UPnP devices found, error: %d", error);
		return false;
	}

	char lanAddr[64] = {};
	int igd = UPNP_GetValidIGD(devlist, &urls, &data, lanAddr, sizeof(lanAddr));
	freeUPNPDevlist(devlist);
	if (igd != 1)
	{
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] No connected UPnP router found");
		if (igd != 0)
			FreeUPNPUrls(&urls);
		return false;
	}

	std::string portStr = StringFromFormat("%d", port);
	int result = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portStr.c_str(), portStr.c_str(), lanAddr,
	                                 "Slippi room", "UDP", nullptr, nullptr);
	if (result != 0)
	{
		WARN_LOG(SLIPPI_ONLINE, "[Rooms] Failed to map port %d with UPnP, error: %d", port, result);
		FreeUPNPUrls(&urls);
		return false;
	}

	INFO_LOG(SLIPPI_ONLINE, "[Rooms] Mapped port %d to %s with UPnP", port, lanAddr);
	return true;
}

// Some routers never remove mappings on their own, so the room always removes its own
void unmapPort(u16 port, UPNPUrls &urls, IGDdatas &data)
{
	std::string portStr = StringFromFormat("%d", port);
	UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, portStr.c_str(), "UDP", nullptr);
	FreeUPNPUrls(&urls);
}
#endif
} // namespace

void SlippiRoomSession::sendMessage(ENetPeer *peer, const json &msg)
{
	std::string contents = msg.dump();
	ENetPacket *packet = enet_packet_create(contents.c_str(), contents.length(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(peer, 0, packet);
}

SlippiRoomHost::SlippiRoomHost(const SlippiExiTypes::CreateRoomQuery &query, const std::string &localName,
                               const std::string &localConnectCode)
    : m_room(query)
    , m_localConnectCode(localConnectCode)
{
	// The host is the room's first member, starting on their last pick
	m_room.AddMember(localName, localConnectCode, query.last_char, query.last_color);

	std::default_random_engine generator(Common::Timer::GetTimeMs());
	for (int i = 0; i < 15 && !m_host; i++)
	{
		m_port = 41000 + (generator() % 10000);

		ENetAddress addr;
		addr.host = ENET_HOST_ANY;
		addr.port = m_port;
		m_host = enet_host_create(&addr, ROOM_MAX_MEMBERS, 1, 0, 0);
	}

	m_registration = std::make_shared<Registration>();
	if (!m_host)
	{
		ERROR_LOG(SLIPPI_ONLINE, "[Rooms] Could not open a port for the room, it can't be joined");
		m_registration->failed = true;
		return;
	}

	SlippiRoomDirectory::RoomInfo info;
	info.port = m_port;
	info.listed = m_room.IsListed();
	info.password = m_room.Password();
	info.hostName = localName;
	info.mode = m_room.Mode();
	info.stageMode = m_room.StageMode();
	info.capacity = m_room.Capacity();

	std::thread(&SlippiRoomHost::directoryThread, m_registration, info, m_port).detach();
	m_netThread = std::thread(&SlippiRoomHost::netThread, this);
}

SlippiRoomHost::~SlippiRoomHost()
{
	m_running = false;
	if (m_netThread.joinable())
		m_netThread.join();

	// The directory thread unregisters the room on its own time
	m_registration->running = false;

	if (m_host)
	{
		for (auto &it : m_peerCodes)
			enet_peer_disconnect(it.first, 0);
		enet_host_flush(m_host);
		enet_host_destroy(m_host);
	}
}

void SlippiRoomHost::HandleLocalAction(u8 action, u8 value0, u8 value1)
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_room.HandleAction(m_room.FindMember(m_localConnectCode), action, value0, value1);
}

void SlippiRoomHost::ReportMatchResult(SlippiRoom::MatchResult result)
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_room.ReportMatchResult(m_room.FindMember(m_localConnectCode), result);
}

SlippiExiTypes::GetRoomStateResponse SlippiRoomHost::GetState()
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_room.Update();

	SlippiExiTypes::GetRoomStateResponse resp = m_room.GetState(m_room.FindMember(m_localConnectCode));
	resp.connection_status = STATUS_HOSTING;

	std::lock_guard<std::mutex> rlk(m_registration->lock);
	resp.connection_error = m_registration->failed ? CONNECT_UNAVAILABLE : CONNECT_OK;
	return resp;
}

void SlippiRoomHost::AddTestPlayer()
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_room.AddTestPlayer();
}

void SlippiRoomHost::netThread()
{
	while (m_running)
	{
		ENetEvent netEvent;
		int net = enet_host_service(m_host, &netEvent, SERVICE_INTERVAL_MS);
		if (net > 0)
		{
			switch (netEvent.type)
			{
			case ENET_EVENT_TYPE_RECEIVE:
			{
				std::string contents(reinterpret_cast<char *>(netEvent.packet->data), netEvent.packet->dataLength);
				enet_packet_destroy(netEvent.packet);

				json msg = json::parse(contents, nullptr, false);
				if (!msg.is_discarded())
					onReceive(netEvent.peer, msg);
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				std::lock_guard<std::mutex> lk(m_lock);
				auto it = m_peerCodes.find(netEvent.peer);
				if (it != m_peerCodes.end())
				{
					m_room.RemoveMember(m_room.FindMember(it->second));
					m_peerCodes.erase(it);
				}
				break;
			}
			default:
				break;
			}
		}

		std::lock_guard<std::mutex> lk(m_lock);
		{
			std::lock_guard<std::mutex> rlk(m_registration->lock);
			m_room.SetCode(m_registration->reg.code);
			m_registration->memberCount = static_cast<u8>(m_room.MemberCount());
		}
		m_room.Update();
		broadcastIfChanged();
	}
}

void SlippiRoomHost::directoryThread(std::shared_ptr<Registration> registration, SlippiRoomDirectory::RoomInfo info,
                                     u16 port)
{
	auto directory = SlippiRoomDirectory::Create();

#ifdef USE_UPNP
	UPNPUrls urls = {};
	IGDdatas data = {};
	bool mapped = mapPort(port, urls, data);
#endif

	SlippiRoomDirectory::Registration reg;
	u32 lastAttemptMs = 0;
	bool firstAttempt = true;

	while (registration->running)
	{
		u32 now = Common::Timer::GetTimeMs();
		if (!firstAttempt && now - lastAttemptMs < SlippiRoomDirectory::HEARTBEAT_INTERVAL_MS)
		{
			Common::SleepCurrentThread(100);
			continue;
		}
		firstAttempt = false;
		lastAttemptMs = now;

		u8 memberCount;
		{
			std::lock_guard<std::mutex> lk(registration->lock);
			memberCount = registration->memberCount;
		}

		// Register the room, or register it again if it expired, such as after the computer slept
		if (reg.code.empty() || !directory->Heartbeat(reg, memberCount))
		{
			reg = directory->Register(info);
			if (!reg.code.empty())
				INFO_LOG(SLIPPI_ONLINE, "[Rooms] Registered room %s", reg.code.c_str());

			std::lock_guard<std::mutex> lk(registration->lock);
			registration->reg = reg;
			registration->failed = reg.code.empty();
		}
	}

	if (!reg.code.empty())
		directory->Unregister(reg);

#ifdef USE_UPNP
	if (mapped)
		unmapPort(port, urls, data);
#endif
}

void SlippiRoomHost::onReceive(ENetPeer *peer, const json &msg)
{
	std::lock_guard<std::mutex> lk(m_lock);
	std::string type = msg.value("type", "");

	if (type == "hello")
	{
		std::string connectCode = msg.value("connectCode", "");
		if (msg.value("version", 0) != PROTOCOL_VERSION)
			return reject(peer, "version");
		if (!m_room.Password().empty() && msg.value("password", "") != m_room.Password())
			return reject(peer, "wrong_password");
		if (connectCode.empty() || m_room.FindMember(connectCode) >= 0)
			return reject(peer, "already_joined");
		if (m_room.AddMember(msg.value("name", ""), connectCode,
		                     msg.value("lastChar", static_cast<int>(SlippiRoom::CHAR_RANDOM)),
		                     msg.value("lastColor", 0)) < 0)
			return reject(peer, "full");

		m_peerCodes[peer] = connectCode;

		// Make sure the new member gets the room right away
		m_lastBroadcast.clear();
		return;
	}

	auto it = m_peerCodes.find(peer);
	if (it == m_peerCodes.end())
		return;

	int member = m_room.FindMember(it->second);
	if (type == "action")
	{
		u8 action = msg.value("action", 0xFF);

		// Test players and leaving are only done by the host, leaving by disconnecting
		if (action == SlippiRoom::ACTION_ADD_TEST_PLAYER || action == SlippiRoom::ACTION_FINISH_SET)
			return;

		m_room.HandleAction(member, action, msg.value("value0", 0), msg.value("value1", 0));
	}
	else if (type == "result")
	{
		m_room.ReportMatchResult(member, static_cast<SlippiRoom::MatchResult>(msg.value("result", 0)));
	}
}

void SlippiRoomHost::reject(ENetPeer *peer, const std::string &reason)
{
	WARN_LOG(SLIPPI_ONLINE, "[Rooms] Rejected a member, reason: %s", reason.c_str());
	sendMessage(peer, {{"type", "reject"}, {"reason", reason}});
	enet_peer_disconnect_later(peer, 0);
}

// Sends every member the room when anything about it changes, telling each which member they are
void SlippiRoomHost::broadcastIfChanged()
{
	json room = m_room.ToJson();
	std::string contents = room.dump();
	if (contents == m_lastBroadcast)
		return;
	m_lastBroadcast = contents;

	for (auto &it : m_peerCodes)
		sendMessage(it.first, {{"type", "state"}, {"you", m_room.FindMember(it.second)}, {"room", room}});
}

SlippiRoomMember::SlippiRoomMember(const SlippiExiTypes::JoinRoomQuery &query, const std::string &localName,
                                   const std::string &localConnectCode)
{
	m_code = std::string(query.code, strnlen(query.code, sizeof(query.code)));
	m_password = std::string(query.password, strnlen(query.password, sizeof(query.password)));
	m_hello = {
	    {"type", "hello"},
	    {"version", PROTOCOL_VERSION},
	    {"name", localName},
	    {"connectCode", localConnectCode},
	    {"password", m_password},
	    {"lastChar", query.last_char},
	    {"lastColor", query.last_color},
	};

	m_netThread = std::thread(&SlippiRoomMember::netThread, this);
}

SlippiRoomMember::~SlippiRoomMember()
{
	m_running = false;
	if (m_netThread.joinable())
		m_netThread.join();
}

void SlippiRoomMember::HandleLocalAction(u8 action, u8 value0, u8 value1)
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_outgoing.push_back({{"type", "action"}, {"action", action}, {"value0", value0}, {"value1", value1}});
}

void SlippiRoomMember::ReportMatchResult(SlippiRoom::MatchResult result)
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_outgoing.push_back({{"type", "result"}, {"result", result}});
}

SlippiExiTypes::GetRoomStateResponse SlippiRoomMember::GetState()
{
	std::lock_guard<std::mutex> lk(m_lock);

	SlippiExiTypes::GetRoomStateResponse resp = {};
	if (m_status == STATUS_JOINED)
		resp = m_room.GetState(m_localMember);

	resp.is_active = 1;
	resp.connection_status = m_status;
	resp.connection_error = m_error;
	strncpy(resp.code, m_code.c_str(), sizeof(resp.code) - 1);
	return resp;
}

void SlippiRoomMember::fail(ConnectionError error)
{
	std::lock_guard<std::mutex> lk(m_lock);
	m_status = STATUS_FAILED;
	m_error = error;
}

void SlippiRoomMember::netThread()
{
	auto directory = SlippiRoomDirectory::Create();
	SlippiRoomDirectory::JoinResult joinResult = directory->Join(m_code, m_password);

	switch (joinResult.status)
	{
	case SlippiRoomDirectory::JoinStatus::OK:
		break;
	case SlippiRoomDirectory::JoinStatus::NOT_FOUND:
		return fail(CONNECT_NOT_FOUND);
	case SlippiRoomDirectory::JoinStatus::WRONG_PASSWORD:
		return fail(CONNECT_WRONG_PASSWORD);
	case SlippiRoomDirectory::JoinStatus::FULL:
		return fail(CONNECT_FULL);
	case SlippiRoomDirectory::JoinStatus::LOCKED:
		return fail(CONNECT_LOCKED);
	default:
		return fail(CONNECT_UNAVAILABLE);
	}

	// Rooms connect over IPv4
	std::vector<std::string> parts;
	SplitString(joinResult.address, ':', parts);
	if (parts.size() != 2)
		return fail(CONNECT_UNREACHABLE);

	std::string host = parts[0];
	u16 port = static_cast<u16>(std::atoi(parts[1].c_str()));

#ifdef LOCAL_TESTING
	// Test clients on one computer can't reach each other through their shared public address
	host = "127.0.0.1";
#endif

	ENetHost *client = enet_host_create(nullptr, 1, 1, 0, 0);
	if (!client)
		return fail(CONNECT_UNREACHABLE);

	ENetAddress addr;
	enet_address_set_host(&addr, host.c_str());
	addr.port = port;
	ENetPeer *peer = enet_host_connect(client, &addr, 1, 0);

	INFO_LOG(SLIPPI_ONLINE, "[Rooms] Connecting to room %s", m_code.c_str());

	bool connected = false;
	u32 startMs = Common::Timer::GetTimeMs();
	while (m_running && !connected && Common::Timer::GetTimeMs() - startMs < CONNECT_TIMEOUT_MS)
	{
		ENetEvent netEvent;
		if (enet_host_service(client, &netEvent, SERVICE_INTERVAL_MS) > 0 && netEvent.type == ENET_EVENT_TYPE_CONNECT)
			connected = true;
	}

	if (!connected)
	{
		enet_host_destroy(client);
		return fail(CONNECT_UNREACHABLE);
	}

	sendMessage(peer, m_hello);

	while (m_running)
	{
		ENetEvent netEvent;
		int net = enet_host_service(client, &netEvent, SERVICE_INTERVAL_MS);
		if (net > 0 && netEvent.type == ENET_EVENT_TYPE_RECEIVE)
		{
			std::string contents(reinterpret_cast<char *>(netEvent.packet->data), netEvent.packet->dataLength);
			enet_packet_destroy(netEvent.packet);

			json msg = json::parse(contents, nullptr, false);
			std::string type = msg.is_discarded() ? "" : msg.value("type", "");
			if (type == "state")
			{
				std::lock_guard<std::mutex> lk(m_lock);
				m_room.FromJson(msg.value("room", json::object()));
				m_localMember = msg.value("you", 0);
				m_status = STATUS_JOINED;
			}
			else if (type == "reject")
			{
				std::string reason = msg.value("reason", "");
				WARN_LOG(SLIPPI_ONLINE, "[Rooms] The room rejected us, reason: %s", reason.c_str());

				ConnectionError error = CONNECT_REJECTED;
				if (reason == "full")
					error = CONNECT_FULL;
				else if (reason == "wrong_password")
					error = CONNECT_WRONG_PASSWORD;
				fail(error);
			}
		}
		else if (net > 0 && netEvent.type == ENET_EVENT_TYPE_DISCONNECT)
		{
			std::lock_guard<std::mutex> lk(m_lock);
			if (m_status != STATUS_FAILED)
			{
				m_status = STATUS_FAILED;
				m_error = CONNECT_DISCONNECTED;
			}
			break;
		}

		std::lock_guard<std::mutex> lk(m_lock);
		while (!m_outgoing.empty())
		{
			sendMessage(peer, m_outgoing.front());
			m_outgoing.pop_front();
		}
	}

	enet_peer_disconnect(peer, 0);
	enet_host_flush(client);
	enet_host_destroy(client);
}
