#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <enet/enet.h>
#include <json.hpp>

#include "Common/CommonTypes.h"
#include "Core/Slippi/SlippiExiTypes.h"
#include "Core/Slippi/SlippiRoom.h"
#include "Core/Slippi/SlippiRoomDirectory.h"

// Being in a room, either as the host running it or as a member who joined it. Members connect
// straight to the host, which sends everyone the room whenever it changes and applies what each
// member does. Matches and spectating connect separately
class SlippiRoomSession
{
  public:
	enum ConnectionStatus : u8
	{
		STATUS_HOSTING = 0,
		STATUS_JOINING = 1,
		STATUS_JOINED = 2,
		STATUS_FAILED = 3,
	};

	enum ConnectionError : u8
	{
		CONNECT_OK = 0,
		CONNECT_UNAVAILABLE = 1, // The rooms directory couldn't be reached
		CONNECT_NOT_FOUND = 2,
		CONNECT_WRONG_PASSWORD = 3,
		CONNECT_FULL = 4,
		CONNECT_LOCKED = 5, // Too many wrong codes or passwords
		CONNECT_UNREACHABLE = 6,
		CONNECT_DISCONNECTED = 7,
		CONNECT_REJECTED = 8,
	};

	static const int PROTOCOL_VERSION = 1;

	virtual ~SlippiRoomSession() = default;

	virtual void HandleLocalAction(u8 action, u8 value0, u8 value1) = 0;
	virtual void ReportMatchResult(SlippiRoom::MatchResult result) = 0;
	virtual SlippiExiTypes::GetRoomStateResponse GetState() = 0;
	virtual void AddTestPlayer() {}

  protected:
	static void sendMessage(ENetPeer *peer, const nlohmann::json &msg);
};

class SlippiRoomHost : public SlippiRoomSession
{
  public:
	SlippiRoomHost(const SlippiExiTypes::CreateRoomQuery &query, const std::string &localName,
	               const std::string &localConnectCode);
	~SlippiRoomHost();

	void HandleLocalAction(u8 action, u8 value0, u8 value1) override;
	void ReportMatchResult(SlippiRoom::MatchResult result) override;
	SlippiExiTypes::GetRoomStateResponse GetState() override;
	void AddTestPlayer() override;

  private:
	// Registering with the directory and keeping the room registered is shared with a thread that
	// can outlive the host, so leaving a room never waits on the network
	struct Registration
	{
		std::mutex lock;
		std::atomic<bool> running{true};
		SlippiRoomDirectory::Registration reg;
		bool failed = false;
		u8 memberCount = 1;
	};

	void netThread();
	static void directoryThread(std::shared_ptr<Registration> registration, SlippiRoomDirectory::RoomInfo info,
	                            u16 port);
	void onReceive(ENetPeer *peer, const nlohmann::json &msg);
	void reject(ENetPeer *peer, const std::string &reason);
	void broadcastIfChanged();

	std::mutex m_lock;
	SlippiRoom m_room;
	std::string m_localConnectCode;

	ENetHost *m_host = nullptr;
	u16 m_port = 0;
	std::map<ENetPeer *, std::string> m_peerCodes; // Members are found by connect code
	std::string m_lastBroadcast;

	std::atomic<bool> m_running{true};
	std::thread m_netThread;
	std::shared_ptr<Registration> m_registration;
};

class SlippiRoomMember : public SlippiRoomSession
{
  public:
	SlippiRoomMember(const SlippiExiTypes::JoinRoomQuery &query, const std::string &localName,
	                 const std::string &localConnectCode);
	~SlippiRoomMember();

	void HandleLocalAction(u8 action, u8 value0, u8 value1) override;
	void ReportMatchResult(SlippiRoom::MatchResult result) override;
	SlippiExiTypes::GetRoomStateResponse GetState() override;

  private:
	void netThread();
	void fail(ConnectionError error);

	std::string m_code;
	std::string m_password;
	nlohmann::json m_hello;

	std::mutex m_lock;
	SlippiRoom m_room;
	int m_localMember = 0;
	ConnectionStatus m_status = STATUS_JOINING;
	ConnectionError m_error = CONNECT_OK;
	std::deque<nlohmann::json> m_outgoing;

	std::atomic<bool> m_running{true};
	std::thread m_netThread;
};
