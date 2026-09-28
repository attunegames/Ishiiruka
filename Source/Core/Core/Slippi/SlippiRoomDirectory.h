#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Where rooms are found. A room's code is turned into its host's address here, only once its
// password checks out, and public rooms are listed. The code outlives its host: when the host leaves
// or is lost, a member takes over and the directory hands out their address instead. Nothing else
// about a room goes through the directory, so any service that can do these six things can back it
class SlippiRoomDirectory
{
  public:
	struct RoomInfo
	{
		u16 port = 0;
		bool listed = false;
		std::string password; // Empty for public rooms
		std::string hostName;
		std::string hostCode;
		u8 mode = 0;
		u8 stageMode = 0;
		u8 capacity = 0;
	};

	// What the host needs to update its room. Empty when registering failed
	struct Registration
	{
		std::string code;
		std::string hostToken;
	};

	enum class ActivityStatus
	{
		OK,
		GONE,     // The room was removed, such as after an hour without activity
		REPLACED, // Another member has taken the room over
		UNAVAILABLE,
	};

	enum class TakeOverStatus
	{
		OK,
		TAKEN, // Another member took over first
		GONE,
		UNAVAILABLE,
	};

	struct TakeOverResult
	{
		TakeOverStatus status = TakeOverStatus::UNAVAILABLE;
		Registration reg;
		int generation = 0;
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
		std::string hostCode;
		int generation = 0; // How many times the room has changed hosts
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

	// The host reports members joining or leaving and sets finishing. Rooms with none of that for an
	// hour are removed
	virtual ActivityStatus Activity(const Registration &reg, u8 memberCount) = 0;

	// A member takes the room over from the host of the given generation
	virtual TakeOverResult TakeOver(const std::string &code, int generation, const RoomInfo &info) = 0;

	virtual void Unregister(const Registration &reg) = 0;
	virtual JoinResult Join(const std::string &code, const std::string &password) = 0;
	virtual bool List(std::vector<Listing> &out) = 0;

	// Returns the directory rooms are configured to use
	static std::unique_ptr<SlippiRoomDirectory> Create();
};
