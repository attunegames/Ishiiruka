#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Where rooms are found. A room's code is turned into its host's address here, only once its
// password checks out, and public rooms are listed. Nothing else about a room goes through the
// directory, so any service that can do these five things can back it
class SlippiRoomDirectory
{
  public:
	struct RoomInfo
	{
		u16 port = 0;
		bool listed = false;
		std::string password; // Empty for public rooms
		std::string hostName;
		u8 mode = 0;
		u8 stageMode = 0;
		u8 capacity = 0; // 0 is no limit
	};

	// What the host needs to keep its room registered. Empty when registering failed
	struct Registration
	{
		std::string code;
		std::string hostToken;
	};

	enum class JoinStatus
	{
		OK,
		NOT_FOUND,
		WRONG_PASSWORD,
		FULL,
		LOCKED,
		UNAVAILABLE, // The directory couldn't be reached or isn't configured
	};

	struct JoinResult
	{
		JoinStatus status = JoinStatus::UNAVAILABLE;
		std::string address; // host:port
		std::string hostName;
	};

	struct Listing
	{
		std::string code;
		std::string hostName;
		u8 mode = 0;
		u8 stageMode = 0;
		u8 capacity = 0;
		u8 memberCount = 0;
	};

	virtual ~SlippiRoomDirectory() = default;

	// These all block on the network, so they should never be called from the CPU thread
	virtual Registration Register(const RoomInfo &info) = 0;
	virtual bool Heartbeat(const Registration &reg, u8 memberCount) = 0;
	virtual void Unregister(const Registration &reg) = 0;
	virtual JoinResult Join(const std::string &code, const std::string &password) = 0;
	virtual bool List(std::vector<Listing> &out) = 0;

	// How often a host should check in to keep its room registered
	static const int HEARTBEAT_INTERVAL_MS = 20000;

	// Returns the directory rooms are configured to use
	static std::unique_ptr<SlippiRoomDirectory> Create();
};
