#include "SlippiRoom.h"

#include <algorithm>
#include <cstring>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"

using json = nlohmann::json;

// Defined with the EXI device, which converts every other connect code for the game
std::string ConvertConnectCodeForGame(const std::string &input);

SlippiRoom::SlippiRoom()
{
	generator = std::default_random_engine(static_cast<unsigned int>(Common::Timer::GetTimeMs()));
}

SlippiRoom::SlippiRoom(const SlippiExiTypes::CreateRoomQuery &query)
    : SlippiRoom()
{
	visibility = query.visibility;
	mode = query.mode;
	capacity = query.capacity;
	stageMode = query.stage_mode;

	// Only private rooms have a password
	if (visibility != 0)
		password = StringFromFormat("%04d", static_cast<int>(generator() % 10000));

	phaseStartMs = Common::Timer::GetTimeMs();
}

int SlippiRoom::AddMember(const std::string &name, const std::string &connectCode, u8 charId, u8 charColor)
{
	if (IsFull())
		return -1;

	Member m;
	m.name = name;
	m.connectCode = connectCode;
	m.charId = charId;
	m.charColor = charColor;
	members.push_back(m);

	INFO_LOG(SLIPPI_ONLINE, "[Room] %s joined the room", connectCode.c_str());
	return static_cast<int>(members.size()) - 1;
}

// Everything that refers to a member by index is shifted down past the one that left
void SlippiRoom::RemoveMember(int member)
{
	if (member < 0 || member >= static_cast<int>(members.size()))
		return;

	INFO_LOG(SLIPPI_ONLINE, "[Room] %s left the room", members[member].connectCode.c_str());

	// Leaving ends any set the member is in. The other player stays on as the winner
	if (isOnSide(member))
	{
		if (sides[SIDE_WINNER] == member)
		{
			sides[SIDE_WINNER] = sides[SIDE_CHALLENGER];
			hasPicked[SIDE_WINNER] = hasPicked[SIDE_CHALLENGER];
			streak = 0;
			beaten = 0;
		}
		sides[SIDE_CHALLENGER] = -1;
		phase = PHASE_WAITING;
	}

	queue.erase(std::remove(queue.begin(), queue.end(), member), queue.end());
	if (crowned == member)
		crowned = -1;

	members.erase(members.begin() + member);
	for (int &q : queue)
	{
		if (q > member)
			q--;
	}
	for (int &s : sides)
	{
		if (s > member)
			s--;
	}
	if (crowned > member)
		crowned--;

	u32 lower = beaten & ((1u << member) - 1);
	u32 upper = member < 31 ? beaten >> (member + 1) : 0;
	beaten = lower | (upper << member);

	fillSides();
	if (phase == PHASE_WAITING)
		phaseStartMs = Common::Timer::GetTimeMs();
}

// Test players take their own turns
void SlippiRoom::AddTestPlayer()
{
	int n = MemberCount() + 1;
	int member = AddMember(StringFromFormat("Player %d", n), StringFromFormat("TEST#%d", n));
	if (member < 0)
		return;

	members[member].isTestPlayer = true;
	joinQueue(member);
}

int SlippiRoom::FindMember(const std::string &connectCode)
{
	for (size_t i = 0; i < members.size(); i++)
	{
		if (members[i].connectCode == connectCode)
			return static_cast<int>(i);
	}
	return -1;
}

int SlippiRoom::MemberCount()
{
	return static_cast<int>(members.size());
}

bool SlippiRoom::IsFull()
{
	int maxMembers = MAX_MEMBERS;
	int limit = capacity == 0 ? maxMembers : std::min<int>(capacity, maxMembers);
	return static_cast<int>(members.size()) >= limit;
}

void SlippiRoom::HandleAction(int member, u8 action, u8 value0, u8 value1)
{
	if (isCopy || member < 0 || member >= static_cast<int>(members.size()))
		return;

	Member &m = members[member];
	Side turn = turnSide();
	bool isTurn = turn != SIDE_NONE && sides[turn] == member;

	switch (action)
	{
	case ACTION_JOIN_QUEUE:
		joinQueue(member);
		break;
	case ACTION_LEAVE_QUEUE:
		leaveQueue(member);
		break;
	case ACTION_STRIKE:
		if (phase == PHASE_STRIKING && isTurn && value0 < STAGE_COUNT)
			strike(value0);
		break;
	case ACTION_CHOOSE:
		if (phase == PHASE_CHOOSING && isTurn && value0 < STAGE_COUNT)
			choose(value0);
		break;
	case ACTION_PICK:
		if (phase == PHASE_PICKING && isTurn)
			pick(turn, value0, value1);
		break;
	case ACTION_SET_COLOR:
		if (isOnSide(member) && phase != PHASE_PLAYING && m.charId < CHAR_RANDOM)
			m.charColor = value0 % maxColors(m.charId);
		break;
	case ACTION_MATCH_ENDED:
		// Nothing reported a result, such as after a disconnect, so the pair plays again
		if (phase == PHASE_PLAYING && isOnSide(member))
		{
			WARN_LOG(SLIPPI_ONLINE, "[Room] Match ended without a result, replaying it");
			phase = PHASE_WAITING;
			phaseStartMs = Common::Timer::GetTimeMs();
		}
		break;
	case ACTION_FINISH_SET:
		if (phase == PHASE_PLAYING && value0 < 2)
			finishSet(static_cast<Side>(value0));
		break;
	}
}

void SlippiRoom::ReportMatchResult(int member, MatchResult result)
{
	if (isCopy || phase != PHASE_PLAYING || !isOnSide(member))
		return;

	// Replay a draw with the same pair
	if (result == RESULT_DRAW)
	{
		INFO_LOG(SLIPPI_ONLINE, "[Room] Match was a draw, replaying it");
		phase = PHASE_WAITING;
		phaseStartMs = Common::Timer::GetTimeMs();
		return;
	}

	Side memberSide = sides[SIDE_WINNER] == member ? SIDE_WINNER : SIDE_CHALLENGER;
	Side otherSide = memberSide == SIDE_WINNER ? SIDE_CHALLENGER : SIDE_WINNER;

	INFO_LOG(SLIPPI_ONLINE, "[Room] Match finished, %s %s", members[member].connectCode.c_str(),
	         result == RESULT_WON ? "won" : "lost");
	finishSet(result == RESULT_WON ? memberSide : otherSide);
}

void SlippiRoom::Update()
{
	if (isCopy)
		return;

	u32 now = Common::Timer::GetTimeMs();

	// Start the set once both sides are filled and the delay has passed
	if (phase == PHASE_WAITING)
	{
		u32 delay = crowned >= 0 ? CROWN_DELAY_MS : SET_DELAY_MS;
		if (sides[SIDE_CHALLENGER] >= 0 && now - phaseStartMs >= delay)
			startSet();
		return;
	}

	Side side = turnSide();
	if (side == SIDE_NONE)
		return;

	// Complete the turn with the first open stage or the member's last character once time
	// runs out, after the same grace period ranked uses
	if (now - turnStartMs > (TURN_SECONDS + GRACE_SECONDS) * 1000)
	{
		WARN_LOG(SLIPPI_ONLINE, "[Room] Turn timed out for side %d", side);
		Member &m = members[sides[side]];
		if (phase == PHASE_STRIKING)
			strike(firstOpenStage());
		else if (phase == PHASE_CHOOSING)
			choose(firstOpenStage());
		else if (phase == PHASE_PICKING)
			pick(side, m.charId, m.charColor);
		return;
	}

	if (members[sides[side]].isTestPlayer && now - turnStartMs >= TEST_PLAYER_DELAY_MS)
		takeTestPlayerTurn(side);
}

json SlippiRoom::ToJson()
{
	json jMembers = json::array();
	for (const Member &m : members)
	{
		jMembers.push_back({{"name", m.name},
		                    {"connectCode", m.connectCode},
		                    {"char", m.charId},
		                    {"color", m.charColor},
		                    {"crowns", m.crowns}});
	}

	return {
	    {"visibility", visibility},
	    {"mode", mode},
	    {"capacity", capacity},
	    {"stageMode", stageMode},
	    {"code", roomCode},
	    {"password", password},
	    {"members", jMembers},
	    {"queue", queue},
	    {"sides", {sides[0], sides[1]}},
	    {"streak", streak},
	    {"crowned", crowned},
	    {"phase", phase},
	    {"struck", std::vector<bool>(struck, struck + STAGE_COUNT)},
	    {"stage", stageIdx},
	    {"hasPicked", {hasPicked[0], hasPicked[1]}},
	    {"playChar", {playChar[0], playChar[1]}},
	    {"playColor", {playColor[0], playColor[1]}},
	    {"turnSeconds", turnSeconds()},
	};
}

void SlippiRoom::FromJson(const json &j)
{
	isCopy = true;

	visibility = j.value("visibility", 0);
	mode = j.value("mode", 0);
	capacity = j.value("capacity", 0);
	stageMode = j.value("stageMode", 0);
	roomCode = j.value("code", "");
	password = j.value("password", "");

	members.clear();
	for (const json &jm : j.value("members", json::array()))
	{
		Member m;
		m.name = jm.value("name", "");
		m.connectCode = jm.value("connectCode", "");
		m.charId = jm.value("char", static_cast<int>(CHAR_RANDOM));
		m.charColor = jm.value("color", 0);
		m.crowns = jm.value("crowns", 0);
		members.push_back(m);
	}

	queue = j.value("queue", std::vector<int>());
	std::vector<int> jSides = j.value("sides", std::vector<int>{-1, -1});
	std::vector<bool> jStruck = j.value("struck", std::vector<bool>());
	std::vector<bool> jHasPicked = j.value("hasPicked", std::vector<bool>());
	std::vector<int> jPlayChar = j.value("playChar", std::vector<int>());
	std::vector<int> jPlayColor = j.value("playColor", std::vector<int>());
	for (int i = 0; i < 2; i++)
	{
		sides[i] = i < static_cast<int>(jSides.size()) ? jSides[i] : -1;
		hasPicked[i] = i < static_cast<int>(jHasPicked.size()) && jHasPicked[i];
		playChar[i] = i < static_cast<int>(jPlayChar.size()) ? jPlayChar[i] : 0;
		playColor[i] = i < static_cast<int>(jPlayColor.size()) ? jPlayColor[i] : 0;
	}
	for (int i = 0; i < STAGE_COUNT; i++)
		struck[i] = i < static_cast<int>(jStruck.size()) && jStruck[i];

	streak = j.value("streak", 0);
	crowned = j.value("crowned", -1);
	phase = static_cast<Phase>(j.value("phase", 0));
	stageIdx = j.value("stage", 0);
	copyTurnSeconds = j.value("turnSeconds", -1);
}

SlippiExiTypes::GetRoomStateResponse SlippiRoom::GetState(int localMember)
{
	SlippiExiTypes::GetRoomStateResponse resp = {};
	resp.is_active = 1;
	resp.visibility = visibility;
	resp.mode = mode;
	resp.capacity = capacity;
	resp.stage_mode = stageMode;
	strncpy(resp.code, roomCode.c_str(), sizeof(resp.code) - 1);
	strncpy(resp.password, password.c_str(), sizeof(resp.password) - 1);
	resp.local_member = static_cast<u8>(localMember);

	resp.member_count = static_cast<u8>(members.size());
	for (size_t i = 0; i < members.size(); i++)
	{
		SlippiExiTypes::RoomMember &rm = resp.members[i];
		std::string name = ConvertStringForGame(members[i].name, 15);
		std::string connectCode = ConvertConnectCodeForGame(members[i].connectCode);
		memcpy(rm.name, name.c_str(), std::min(name.size(), sizeof(rm.name) - 1));
		memcpy(rm.connect_code, connectCode.c_str(), std::min(connectCode.size(), sizeof(rm.connect_code) - 1));
		rm.char_id = members[i].charId;
		rm.char_color = members[i].charColor;
		rm.crowns = members[i].crowns;
	}

	resp.queue_count = static_cast<u8>(queue.size());
	for (size_t i = 0; i < queue.size(); i++)
		resp.queue[i] = static_cast<u8>(queue[i]);

	for (int i = 0; i < 2; i++)
	{
		resp.sides[i] = static_cast<s8>(sides[i]);
		resp.has_picked[i] = hasPicked[i];
		resp.play_char[i] = playChar[i];
		resp.play_color[i] = playColor[i];
	}

	resp.streak = streak;
	resp.crowned = static_cast<s8>(crowned);
	resp.phase = phase;
	for (int i = 0; i < STAGE_COUNT; i++)
		resp.struck[i] = struck[i];
	resp.stage_idx = static_cast<u8>(stageIdx);

	int seconds = turnSeconds();
	resp.turn_seconds = seconds < 0 ? 0xFF : static_cast<u8>(seconds);

	return resp;
}

int SlippiRoom::queuePos(int member)
{
	for (size_t i = 0; i < queue.size(); i++)
	{
		if (queue[i] == member)
			return static_cast<int>(i);
	}
	return -1;
}

bool SlippiRoom::isOnSide(int member)
{
	return sides[SIDE_WINNER] == member || sides[SIDE_CHALLENGER] == member;
}

void SlippiRoom::joinQueue(int member)
{
	if (queuePos(member) >= 0 || isOnSide(member))
		return;

	queue.push_back(member);
	fillSides();

	// Restart the delay before a set so a new challenger is seen before it starts
	if (phase == PHASE_WAITING)
		phaseStartMs = Common::Timer::GetTimeMs();
}

void SlippiRoom::leaveQueue(int member)
{
	int pos = queuePos(member);
	if (pos >= 0)
		queue.erase(queue.begin() + pos);

	// Also give up the member's side if the set hasn't started yet
	if (phase == PHASE_WAITING && isOnSide(member))
	{
		if (sides[SIDE_WINNER] == member)
		{
			sides[SIDE_WINNER] = sides[SIDE_CHALLENGER];
			hasPicked[SIDE_WINNER] = hasPicked[SIDE_CHALLENGER];
			streak = 0;
			beaten = 0;
		}
		sides[SIDE_CHALLENGER] = -1;
		fillSides();
	}

	if (phase == PHASE_WAITING)
		phaseStartMs = Common::Timer::GetTimeMs();
}

// Moves the next players in the queue onto any empty side between sets
void SlippiRoom::fillSides()
{
	if (phase != PHASE_WAITING)
		return;

	for (int side = 0; side < 2; side++)
	{
		if (sides[side] >= 0 || queue.empty())
			continue;

		sides[side] = queue.front();
		hasPicked[side] = false;
		queue.erase(queue.begin());
	}
}

void SlippiRoom::startSet()
{
	memset(struck, 0, sizeof(struck));
	memset(hasPicked, 0, sizeof(hasPicked));
	crowned = -1;

	if (stageMode != 0)
	{
		phase = PHASE_STRIKING;
	}
	else
	{
		stageIdx = generator() % STAGE_COUNT;
		phase = PHASE_PICKING;
	}

	startTurn();
}

// The winner stays on and the loser goes to the back of the queue
void SlippiRoom::finishSet(Side winner)
{
	int winningMember = sides[winner];
	int losingMember = sides[winner == SIDE_WINNER ? SIDE_CHALLENGER : SIDE_WINNER];

	if (winner == SIDE_WINNER)
	{
		streak++;
		beaten |= 1u << losingMember;
	}
	else
	{
		streak = 1;
		beaten = 1u << losingMember;
	}
	sides[SIDE_WINNER] = winningMember;
	sides[SIDE_CHALLENGER] = -1;
	phase = PHASE_WAITING;

	// Keep showing the winner's character from the last set
	hasPicked[SIDE_WINNER] = true;

	// Beating everyone in the queue earns a crown and sends the winner to the back of it,
	// behind the member they just beat. Checked before the loser rejoins, since that fills
	// the open side
	bool isCrowned = hasBeatenEveryone();
	if (isCrowned)
	{
		members[winningMember].crowns++;
		crowned = winningMember;
		streak = 0;
		beaten = 0;
		sides[SIDE_WINNER] = -1;
	}

	joinQueue(losingMember);
	if (isCrowned)
		joinQueue(winningMember);

	phaseStartMs = Common::Timer::GetTimeMs();
}

bool SlippiRoom::hasBeatenEveryone()
{
	for (int member : queue)
	{
		if (!(beaten & (1u << member)))
			return false;
	}
	return true;
}

SlippiRoom::Side SlippiRoom::turnSide()
{
	if (phase == PHASE_STRIKING)
		return SIDE_WINNER;
	if (phase == PHASE_CHOOSING)
		return SIDE_CHALLENGER;
	if (phase == PHASE_PICKING)
		return hasPicked[SIDE_WINNER] ? SIDE_CHALLENGER : SIDE_WINNER;
	return SIDE_NONE;
}

// Seconds left in the current turn, -1 when it's nobody's turn
int SlippiRoom::turnSeconds()
{
	if (isCopy)
		return copyTurnSeconds;
	if (turnSide() == SIDE_NONE)
		return -1;

	int remaining = TURN_SECONDS - static_cast<int>((Common::Timer::GetTimeMs() - turnStartMs) / 1000);
	return remaining < 0 ? 0 : remaining;
}

int SlippiRoom::firstOpenStage()
{
	for (int i = 0; i < STAGE_COUNT; i++)
	{
		if (!struck[i])
			return i;
	}
	return 0;
}

// In the draft the winner strikes a stage, then the challenger chooses from the rest
void SlippiRoom::strike(int idx)
{
	struck[idx] = true;
	phase = PHASE_CHOOSING;
	startTurn();
}

void SlippiRoom::choose(int idx)
{
	if (struck[idx])
		return;

	stageIdx = idx;
	phase = PHASE_PICKING;
	startTurn();
}

void SlippiRoom::pick(Side side, u8 charId, u8 charColor)
{
	Member &m = members[sides[side]];
	m.charId = charId > CHAR_RANDOM ? CHAR_RANDOM : charId;
	m.charColor = m.charId < CHAR_RANDOM ? charColor % maxColors(m.charId) : 0;
	hasPicked[side] = true;

	if (hasPicked[SIDE_CHALLENGER])
		startMatch();
	else
		startTurn();
}

// Random picks become a random character and color once the match starts
void SlippiRoom::startMatch()
{
	for (int side = 0; side < 2; side++)
	{
		Member &m = members[sides[side]];
		playChar[side] = m.charId;
		playColor[side] = m.charColor;

		if (m.charId >= CHAR_RANDOM)
		{
			playChar[side] = generator() % CHAR_RANDOM;
			playColor[side] = generator() % maxColors(playChar[side]);
		}
	}

	phase = PHASE_PLAYING;
	INFO_LOG(SLIPPI_ONLINE, "[Room] Match starting: %d vs %d on stage %d", playChar[0], playChar[1], stageIdx);
}

void SlippiRoom::startTurn()
{
	turnStartMs = Common::Timer::GetTimeMs();
}

void SlippiRoom::takeTestPlayerTurn(Side side)
{
	if (phase == PHASE_STRIKING || phase == PHASE_CHOOSING)
	{
		int idx = generator() % STAGE_COUNT;
		while (struck[idx])
			idx = (idx + 1) % STAGE_COUNT;

		if (phase == PHASE_STRIKING)
			strike(idx);
		else
			choose(idx);
	}
	else if (phase == PHASE_PICKING)
	{
		pick(side, generator() % (CHAR_RANDOM + 1), 0); // Includes random
	}
}

// Same color counts as ranked's vanilla colors
u8 SlippiRoom::maxColors(u8 charId)
{
	static const u8 counts[CHAR_RANDOM] = {6, 5, 4, 4, 6, 4, 5, 4, 5, 5, 4, 4, 5,
	                                       4, 4, 5, 5, 6, 5, 5, 4, 5, 5, 5, 4, 5};
	return charId < CHAR_RANDOM ? counts[charId] : 1;
}
