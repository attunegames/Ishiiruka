#pragma once

#include <random>
#include <string>
#include <vector>

#include <json.hpp>

#include "Common/CommonTypes.h"
#include "Core/Slippi/SlippiExiTypes.h"

// A room of players taking turns on one setup. Winner stays on, the loser goes to the back of the
// queue, and beating everyone in the queue earns a crown.
//
// The host's copy runs the room. Every member keeps a copy of the host's, read from what the host
// sends, which is only ever shown and never run
class SlippiRoom
{
  public:
	enum Side : s8
	{
		SIDE_NONE = -1,
		SIDE_WINNER = 0,
		SIDE_CHALLENGER = 1,
	};

	enum Phase : u8
	{
		PHASE_WAITING = 0,
		PHASE_STRIKING = 1,
		PHASE_CHOOSING = 2,
		PHASE_PICKING = 3,
		PHASE_PLAYING = 4,
	};

	enum Action : u8
	{
		ACTION_JOIN_QUEUE = 0,
		ACTION_LEAVE_QUEUE = 1,
		ACTION_STRIKE = 2,
		ACTION_CHOOSE = 3,
		ACTION_PICK = 4,
		ACTION_SET_COLOR = 5,
		ACTION_MATCH_ENDED = 6,
		ACTION_ADD_TEST_PLAYER = 7,
		ACTION_FINISH_SET = 8,
		ACTION_LEAVE_ROOM = 9,
	};

	enum MatchResult : s8
	{
		RESULT_DRAW = -1,
		RESULT_LOST = 0,
		RESULT_WON = 1,
	};

	static const int MAX_MEMBERS = ROOM_MAX_MEMBERS;
	static const int STAGE_COUNT = ROOM_STAGE_COUNT;
	static const u8 CHAR_RANDOM = 26;

	static const int SET_DELAY_MS = 2000;
	static const int CROWN_DELAY_MS = 4000;
	static const int TEST_PLAYER_DELAY_MS = 1000;
	static const int TURN_SECONDS = 30;
	static const int GRACE_SECONDS = 3;

	SlippiRoom();
	SlippiRoom(const SlippiExiTypes::CreateRoomQuery &query);

	// Returns the new member's index, -1 if the room is full
	int AddMember(const std::string &name, const std::string &connectCode, u8 charId = CHAR_RANDOM, u8 charColor = 0);
	void RemoveMember(int member);
	void AddTestPlayer();
	int FindMember(const std::string &connectCode);
	int MemberCount();
	bool IsFull();

	void HandleAction(int member, u8 action, u8 value0, u8 value1);
	void ReportMatchResult(int member, MatchResult result);
	void Update();

	void SetCode(const std::string &code) { roomCode = code; }
	const std::string &Password() { return password; }
	bool IsListed() { return visibility == 0; }
	u8 Mode() { return mode; }
	u8 StageMode() { return stageMode; }
	u8 Capacity() { return capacity; }
	std::string HostName() { return members.empty() ? "" : members[0].name; }

	// What the host sends its members, and how a member's copy is made from it
	nlohmann::json ToJson();
	void FromJson(const nlohmann::json &j);

	SlippiExiTypes::GetRoomStateResponse GetState(int localMember);

  protected:
	struct Member
	{
		std::string name;
		std::string connectCode;
		u8 charId = CHAR_RANDOM;
		u8 charColor = 0;
		u8 crowns = 0;
		bool isTestPlayer = false;
	};

	int queuePos(int member);
	bool isOnSide(int member);
	void joinQueue(int member);
	void leaveQueue(int member);
	void fillSides();
	void startSet();
	void finishSet(Side winner);
	bool hasBeatenEveryone();
	Side turnSide();
	int turnSeconds();
	int firstOpenStage();
	void strike(int idx);
	void choose(int idx);
	void pick(Side side, u8 charId, u8 charColor);
	void startMatch();
	void startTurn();
	void takeTestPlayerTurn(Side side);
	u8 maxColors(u8 charId);

	u8 visibility = 0;
	u8 mode = 0;
	u8 capacity = 0;
	u8 stageMode = 0;
	std::string roomCode;
	std::string password;

	std::vector<Member> members;
	std::vector<int> queue;
	int sides[2] = {-1, -1};
	u8 streak = 0;
	u32 beaten = 0; // Bit per member the current winner has beaten
	int crowned = -1;
	Phase phase = PHASE_WAITING;
	bool struck[STAGE_COUNT] = {};
	int stageIdx = 0;
	bool hasPicked[2] = {};
	u8 playChar[2] = {};
	u8 playColor[2] = {};

	u32 phaseStartMs = 0;
	u32 turnStartMs = 0;

	// A member's copy shows the host's timer rather than timing turns itself
	bool isCopy = false;
	int copyTurnSeconds = -1;

	std::default_random_engine generator;
};
