-- Rooms, part 2: who is playing, what they picked, and who won.
--
-- This is a PORT, not a new design. The old build reached this over twenty-two
-- migrations, and most of them were fixes for things that are not obvious until
-- three people are stood in a room - queue order that disagreed with pairing
-- order, a heartbeat that quietly moved you down the queue, a winner whose
-- place could be taken by the player they had just beaten. Rewriting it would
-- mean finding all of that again. The comments that explain WHY come across
-- with the code; that is the part worth keeping.
--
-- What changed on the way over, and why:
--
--   searching_at is gone. The old table had it alongside queued_at because
--   queued_at arrived late, to fix a heartbeat that reset your place. Here
--   queued_at is the only queue column from the start - set when you press
--   Start, never touched by a tick - and last_seen already says whether you
--   are still here. Two columns became one and the bug they were fighting
--   cannot happen.
--
--   No LAN. Not a rename, a removal: this is for playing people over the
--   internet, and a LAN shortcut once cost every remote watcher a full connect
--   timeout against an address that was never theirs.
--
--   No spectate address. A watcher attaches to a player's existing netplay
--   socket, so the hole that socket already has is the only one anyone needs.
--
--   external -> addr, endpoint_at -> addr_at, to match part 1.
--
--   winner_stocks is new. The old build recorded who won but never by how much.
--
--   pd_queue_window() from part 1 is deliberately UNUSED. It exists because the
--   old build needed a separate staleness clock for "still searching"; here
--   queued_at is sticky - set on the way in, cleared only by leaving the queue
--   - and last_seen already decides whether you are still in the room at all.
--   One clock instead of two, which is what removes the class of bug where the
--   two disagreed. Left defined rather than dropped: part 1 is already applied.

-- ------------------------------------------------------------------ tables ---

-- One row per arranged match. Made fresh every time, which is what stops last
-- game's picks leaking into the next one - there is nothing to clear.
create table pd_pairings (
  room       text        not null references pd_rooms (code) on delete cascade,
  id         uuid        primary key default gen_random_uuid(),
  match_id   text        not null,

  host       uuid        not null,
  guest      uuid        not null,

  -- pending: arranged, nobody has an address yet.
  -- ready:   both addresses are fresh, the match is on.
  -- done:    over, or abandoned.
  state      text        not null default 'pending'
                         check (state in ('pending', 'ready', 'done')),

  -- What the pair settled on, and what the top of the room draws. Null means
  -- "has not picked yet", which the room shows as a question mark rather than
  -- guessing - so these stay null until someone reports, on purpose.
  --
  -- Each player writes their OWN character; either may write the stage.
  stage       smallint,
  char_host   smallint,
  color_host  smallint,
  char_guest  smallint,
  color_guest smallint,

  winner        uuid,
  winner_stocks smallint,   -- stocks left on the winner, from the game end

  created_at timestamptz not null default now(),
  ended_at   timestamptz
);

create index pd_pairings_open on pd_pairings (room, state)
  where state in ('pending', 'ready');

-- Who each player has beaten in this room, for crowns. A run ends when you
-- lose, which is a delete rather than a flag.
create table pd_beaten (
  room      text not null references pd_rooms (code) on delete cascade,
  player    uuid not null,
  opponent  uuid not null,
  beaten_at timestamptz not null default now(),
  primary key (room, player, opponent)
);

alter table pd_pairings enable row level security;
alter table pd_beaten   enable row level security;
-- No policies, deliberately, exactly as part 1: everything goes through these
-- functions and nothing reads the tables directly.

-- -------------------------------------------------------------------- tick ---

-- Called by Dolphin every couple of seconds while it is in a room. One round
-- trip does everything: says we are still here, publishes our address, reports
-- what we picked, arranges a match if one is due, and hands back the whole room.
--
-- Everything is under an advisory lock on the room. Two clients ticking at the
-- same instant would otherwise each see no open pairing and each make one.
create or replace function pd_tick(
  p_room          text,
  p_name          text,
  p_code          text,
  p_addr          text     default null,   -- "ip:port" as STUN saw us
  p_reset         boolean  default false,
  p_presence_only boolean  default false,  -- in the room, not in the queue
  p_watch_addr    text     default null,
  p_queued        boolean  default null,   -- pressed Start / left the queue
  p_stage         smallint default null,
  p_char          smallint default null,
  p_color         smallint default null
) returns json
language plpgsql security definer set search_path = public as $$
declare
  v_me      uuid := auth.uid();
  v_pair    pd_pairings%rowtype;
  v_show    pd_pairings%rowtype;
  v_other   pd_members%rowtype;
  v_mine    pd_members%rowtype;
  v_partner uuid;
  v_active  json;
  v_queue   json;
  v_lobby   json;
  v_punch   json;
  v_base    json;
  v_draft   json;
  v_position int;
  v_first   uuid;
  v_second  uuid;
begin
  if v_me is null then
    return json_build_object('state', 'error', 'error', 'not signed in');
  end if;

  perform pg_advisory_xact_lock(hashtext(p_room));

  -- Gone quiet for longer than the window: they closed the game.
  delete from pd_members
   where room = p_room and last_seen < now() - pd_presence_window();

  -- A pairing whose player has left is not a match any more, and left alone it
  -- blocks the other one from ever being matched again.
  update pd_pairings pr set state = 'done'
   where pr.room = p_room and pr.state in ('pending', 'ready')
     and (not exists (select 1 from pd_members m
                       where m.room = pr.room and m.player = pr.host)
       or not exists (select 1 from pd_members m
                       where m.room = pr.room and m.player = pr.guest));

  -- An arrangement nobody turned up for. Both sides publish an address within a
  -- couple of ticks of being paired, so one still pending a minute later is
  -- dead - and it would otherwise sit there forever holding two people out of
  -- the queue.
  update pd_pairings set state = 'done'
   where room = p_room and state = 'pending'
     and created_at < now() - interval '60 seconds';

  -- And a match that started and never finished. ⚠️ Only 'pending' was reaped
  -- above, so a pairing that reached 'ready' - both addresses published, the
  -- match on - and then died sat there for good: a draft that deadlocks, a game
  -- that disconnects without a result, a Dolphin closed at the wrong moment.
  -- pd_tick will not pair either player again while that row exists, because it
  -- excludes anyone already in a pairing, so BOTH of them drop out of the
  -- rotation with nothing on screen to say why.
  --
  -- Fifteen minutes is well past a real one: eight for the game clock, a minute
  -- or two for the draft, and the room reports a result the moment it ends.
  update pd_pairings set state = 'done'
   where room = p_room and state = 'ready'
     and created_at < now() - interval '15 minutes';

  -- ⚠️ queued_at is set ON THE WAY IN and never by a heartbeat. Ticking again
  -- while already queued must not move you - that was the bug that had the same
  -- two players rematching over somebody who had been waiting longer.
  insert into pd_members (room, player, name, code, addr, addr_at,
                          watch_addr, watch_at, queued_at)
       values (p_room, v_me, p_name, p_code, p_addr,
               case when p_addr is not null then now() end,
               p_watch_addr,
               case when p_watch_addr is not null then now() end,
               case when p_queued is true then now() end)
  on conflict (room, player) do update
     set name       = excluded.name,
         code       = excluded.code,
         last_seen  = now(),
         addr       = case when p_reset then null
                           else coalesce(excluded.addr, pd_members.addr) end,
         addr_at    = case when p_reset then null
                           when p_addr is not null then now()
                           else pd_members.addr_at end,
         watch_addr = case when p_reset then null
                           else coalesce(p_watch_addr, pd_members.watch_addr) end,
         watch_at   = case when p_watch_addr is not null then now()
                           else pd_members.watch_at end,
         queued_at  = case when p_queued is false then null
                           when p_queued is true  then coalesce(pd_members.queued_at, now())
                           else pd_members.queued_at end;

  if p_reset then
    update pd_pairings set state = 'done'
     where room = p_room and state in ('pending', 'ready')
       and (host = v_me or guest = v_me);
    return json_build_object('state', 'waiting', 'role', 'queued');
  end if;

  -- ----------------------------------------------------------- arranging ---

  select * into v_pair from pd_pairings
   where room = p_room and state in ('pending', 'ready')
     and (host = v_me or guest = v_me)
   order by created_at desc limit 1;

  -- ⚠️ ONE MATCH AT A TIME, and the room-wide check is what enforces it.
  -- The per-player checks below only ask whether each of the two is free. With
  -- four people in a room the two who were NOT playing were both free, so a
  -- second concurrent match started - which breaks the rotation the room exists
  -- for: the winner has nobody to play, and the band can only show one match.
  if v_pair.id is null and not p_presence_only
     and not exists (select 1 from pd_pairings pr
                      where pr.room = p_room
                        and pr.state in ('pending', 'ready')) then
    -- Free = in the queue, still alive, and not already in a match. hold_until
    -- keeps the winner's place while they are still picking a character, so
    -- they count as free even before they press Start again.
    select f.player into v_first from (
      select m.player from pd_members m
       where m.room = p_room
         and (m.queued_at is not null or m.hold_until > now())
         and m.last_seen > now() - pd_presence_window()
         and not exists (select 1 from pd_pairings pr
                          where pr.room = p_room and pr.state in ('pending', 'ready')
                            and (pr.host = m.player or pr.guest = m.player))
       order by coalesce(m.queued_at, now()) asc, m.joined_at asc limit 1) f;

    select s.player into v_second from (
      select m.player from pd_members m
       where m.room = p_room and m.player <> v_first
         and (m.queued_at is not null or m.hold_until > now())
         and m.last_seen > now() - pd_presence_window()
         and not exists (select 1 from pd_pairings pr
                          where pr.room = p_room and pr.state in ('pending', 'ready')
                            and (pr.host = m.player or pr.guest = m.player))
       order by coalesce(m.queued_at, now()) asc, m.joined_at asc limit 1) s;

    -- A slot HELD by someone who has not pressed Start yet blocks the match
    -- rather than letting the next person take it. That is what "the winner
    -- stays" means while the winner is still choosing a character - so both of
    -- the two have to be genuinely queued, not merely holding.
    if v_first is not null and v_second is not null
       and (v_me = v_first or v_me = v_second)
       and exists (select 1 from pd_members m where m.room = p_room
                    and m.player = v_first and m.queued_at is not null)
       and exists (select 1 from pd_members m where m.room = p_room
                    and m.player = v_second and m.queued_at is not null) then
      insert into pd_pairings (room, match_id, host, guest)
      values (p_room, 'pd-' || replace(gen_random_uuid()::text, '-', ''),
              v_first, v_second)
      returning * into v_pair;
    end if;
  end if;

  -- ------------------------------------------------------- what we picked ---
  -- Our own side only, and only into a pairing we are in. The stage can come
  -- from either of the two; the character cannot.
  if v_pair.id is not null then
    if p_stage is not null then
      update pd_pairings set stage = p_stage where id = v_pair.id;
    end if;
    if p_char is not null and v_pair.host = v_me then
      update pd_pairings set char_host = p_char, color_host = coalesce(p_color, 0)
       where id = v_pair.id;
    elsif p_char is not null and v_pair.guest = v_me then
      update pd_pairings set char_guest = p_char, color_guest = coalesce(p_color, 0)
       where id = v_pair.id;
    end if;
    select * into v_pair from pd_pairings where id = v_pair.id;
  end if;

  -- ----------------------------------------------------------- room view ---

  -- Everyone sees the same match at the top, whether they are in it or not -
  -- that is the whole point of the band. Ours if we are in one, otherwise
  -- whatever the room is currently playing.
  v_show := v_pair;
  if v_show.id is null then
    select * into v_show from pd_pairings
     where room = p_room and state in ('pending', 'ready')
     order by (state = 'ready') desc, created_at desc limit 1;
  end if;

  select coalesce(json_agg(x.obj order by x.ord), '[]'::json) into v_active from (
    select 0 as ord, json_build_object('name', m.name, 'code', m.code,
                                       'crowns', m.crowns, 'addr', m.addr) as obj
      from pd_members m where m.room = p_room and m.player = v_show.host
    union all
    select 1, json_build_object('name', m.name, 'code', m.code,
                                'crowns', m.crowns, 'addr', m.addr)
      from pd_members m where m.room = p_room and m.player = v_show.guest) x;

  -- Ordered exactly the way the pairing picks, or the screen shows one queue
  -- and the room plays a different one.
  select coalesce(json_agg(json_build_object('name', m.name, 'code', m.code,
                                             'crowns', m.crowns)
                           order by m.queued_at, m.joined_at), '[]'::json)
    into v_queue
    from pd_members m
   where m.room = p_room
     and m.queued_at is not null
     and m.last_seen > now() - pd_presence_window()
     and not exists (select 1 from pd_pairings pr
                      where pr.room = p_room and pr.state in ('pending', 'ready')
                        and (pr.host = m.player or pr.guest = m.player));

  -- In the room but not waiting for a game. Listed beside the queue.
  select coalesce(json_agg(json_build_object('name', m.name, 'code', m.code,
                                             'crowns', m.crowns)
                           order by m.joined_at), '[]'::json)
    into v_lobby
    from pd_members m
   where m.room = p_room
     and m.queued_at is null
     and m.last_seen > now() - pd_presence_window()
     and not exists (select 1 from pd_pairings pr
                      where pr.room = p_room and pr.state in ('pending', 'ready')
                        and (pr.host = m.player or pr.guest = m.player));

  -- Counted the same way the pair is picked, so "you are next" means it.
  --
  -- Our own two keys come from a join rather than a scalar subquery: a bare
  -- (a, b) in a SELECT list is not a row there, and Postgres rejects it as a
  -- subquery with too few columns.
  select count(*) + 1 into v_position
    from pd_members m,
         (select queued_at as qa, joined_at as ja
            from pd_members where room = p_room and player = v_me) me
   where m.room = p_room
     and m.queued_at is not null
     and (m.queued_at, m.joined_at) < (me.qa, me.ja)
     and m.last_seen > now() - pd_presence_window()
     and not exists (select 1 from pd_pairings pr
                      where pr.room = p_room and pr.state in ('pending', 'ready')
                        and (pr.host = m.player or pr.guest = m.player));

  -- What the top of the room draws.
  --
  -- A null character means "has not picked yet" and stays null - the room shows
  -- a question mark rather than guessing. 'playing' is the difference between
  -- the pair being arranged and the match actually being on, which is what
  -- decides whether the band shows two fighters or stays empty.
  v_draft := json_build_object(
    'stage',      v_show.stage,
    'hostChar',   v_show.char_host,
    'hostColor',  coalesce(v_show.color_host, 0),
    'guestChar',  v_show.char_guest,
    'guestColor', coalesce(v_show.color_guest, 0),
    'playing',    coalesce(v_show.state = 'ready', false));

  v_base := json_build_object(
    'room', p_room, 'active', v_active, 'queue', v_queue, 'lobby', v_lobby,
    'position', v_position, 'draft', v_draft,
    'live', coalesce(v_show.state = 'ready', false));

  -- Anyone in the room trying to watch, so the pair can punch a hole out to
  -- them. The heartbeat is the only thing still running during a match, so it
  -- has to carry this - otherwise a spectator arriving after the first whistle
  -- never gets a hole punched and simply cannot connect.
  select coalesce(json_agg(m.watch_addr), '[]'::json) into v_punch
    from pd_members m
   where m.room = p_room and m.player <> v_me
     and m.watch_addr is not null
     and m.watch_at > now() - pd_presence_window();

  if p_presence_only then
    return (v_base::jsonb || json_build_object(
      'state', 'heartbeat', 'role', 'idle', 'punch', v_punch)::jsonb)::json;
  end if;

  if v_pair.id is null then
    return (v_base::jsonb || json_build_object(
      'state', 'waiting', 'role', 'queued')::jsonb)::json;
  end if;

  v_partner := case when v_pair.host = v_me then v_pair.guest else v_pair.host end;
  select * into v_other from pd_members where room = p_room and player = v_partner;
  select * into v_mine  from pd_members where room = p_room and player = v_me;

  -- Neither side dials an address older than the window: a NAT mapping can
  -- expire, and a stale one is a connection that times out rather than fails.
  if v_mine.addr_at is null or v_mine.addr_at < now() - pd_address_window()
     or v_other.addr_at is null or v_other.addr_at < now() - pd_address_window() then
    return (v_base::jsonb || json_build_object(
      'state', 'stun', 'role', 'player', 'matchId', v_pair.match_id)::jsonb)::json;
  end if;

  update pd_pairings set state = 'ready' where id = v_pair.id and state = 'pending';

  return (v_base::jsonb || json_build_object(
    'state',     'ready',
    'role',      'player',
    'punch',     v_punch,
    'live',      true,
    'matchId',   v_pair.match_id,
    'isHost',    v_pair.host = v_me,
    'stagePick', v_pair.guest = v_me,
    'me',       json_build_object('name', v_mine.name,  'code', v_mine.code,
                                  'addr', v_mine.addr),
    'opponent', json_build_object('name', v_other.name, 'code', v_other.code,
                                  'addr', v_other.addr)
  )::jsonb)::json;
end $$;

grant execute on function pd_tick(text, text, text, text, boolean, boolean,
                                  text, boolean, smallint, smallint, smallint)
  to authenticated;

-- ------------------------------------------------------------------ result ---

-- Reported by either player when the match ends. Whoever gets here first
-- settles it; the other one is told it is already recorded rather than being
-- treated as an error, because both of them will call this.
create or replace function pd_result(
  p_room     text,
  p_match_id text,
  p_i_won    boolean,
  p_stocks   smallint default null   -- stocks left on the winner
) returns json
language plpgsql security definer set search_path = public as $$
declare
  v_me      uuid := auth.uid();
  v_pair    pd_pairings%rowtype;
  v_winner  uuid;
  v_loser   uuid;
  v_others  int;
  v_beaten  int;
  v_crowned boolean := false;
  v_name    text;
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  perform pg_advisory_xact_lock(hashtext(p_room));

  select * into v_pair from pd_pairings
   where room = p_room and match_id = p_match_id
     and state in ('pending', 'ready')
     and (host = v_me or guest = v_me);

  if v_pair.id is null then
    return json_build_object('ok', true, 'note', 'already recorded');
  end if;

  if p_i_won then
    v_winner := v_me;
    v_loser  := case when v_pair.host = v_me then v_pair.guest else v_pair.host end;
  else
    v_loser  := v_me;
    v_winner := case when v_pair.host = v_me then v_pair.guest else v_pair.host end;
  end if;

  update pd_pairings
     set state = 'done', winner = v_winner, winner_stocks = p_stocks, ended_at = now()
   where id = v_pair.id;

  -- The loser's run, if they had one, is over.
  delete from pd_beaten where room = p_room and player = v_loser;
  insert into pd_beaten (room, player, opponent) values (p_room, v_winner, v_loser)
  on conflict (room, player, opponent) do update set beaten_at = now();

  -- Has the winner now beaten everyone who is actually here? People who have
  -- left do not count towards it, and people who have arrived since do.
  select count(*) into v_others
    from pd_members m where m.room = p_room and m.player <> v_winner;

  select count(*) into v_beaten
    from pd_beaten b
    join pd_members m on m.room = b.room and m.player = b.opponent
   where b.room = p_room and b.player = v_winner;

  if v_others >= 2 and v_beaten >= v_others then
    v_crowned := true;
    -- queued_at as well as joined_at, or the champion keeps the front of the
    -- queue they just earned their way out of.
    --
    -- ⚠️ BEHIND the loser, not level with them. now() is the TRANSACTION's
    -- clock in PostgreSQL, not the statement's, so this write and the loser's
    -- at the end of this function got the identical timestamp - and joined_at
    -- tied the same way. The champion was never sent to the back, only made
    -- equal to the person they had just beaten, and with three people that is
    -- enough to hand them the next game on a coin flip.
    update pd_members
       set crowns = crowns + 1,
           joined_at = now() + pd_crown_offset(),
           queued_at = now() + pd_crown_offset()
     where room = p_room and player = v_winner;
    delete from pd_beaten where room = p_room and player = v_winner;

    -- Going to the back has to mean it. A pairing arranged for the champion in
    -- the same moment the crown lands would otherwise stand, and the room would
    -- go on offering them the next game they just earned their way out of.
    update pd_pairings set state = 'done'
     where room = p_room and state in ('pending', 'ready')
       and id <> v_pair.id
       and (host = v_winner or guest = v_winner);
  end if;

  -- The winner keeps the next game if they want it. They press Start like
  -- anybody else, and until they do the room can see only the people who
  -- already have - so without this the player who just LOST could press Start
  -- first and take the winner's place in the very next game.
  if not v_crowned then
    update pd_members set hold_until = now() + interval '45 seconds'
     where room = p_room and player = v_winner;
  end if;

  -- The rotation, in one line: the loser becomes the newest arrival, so the
  -- winner is the longest-waiting free member and pd_tick pairs them with
  -- whoever is genuinely next. A crowned winner was sent to the back above,
  -- which puts BOTH of them behind everyone waiting.
  --
  -- ⚠️ queued_at as well as joined_at. queued_at is what the pairing, the queue
  -- list and "you are next" all order by, and moving joined_at ALONE is what
  -- let the same two players re-pair over somebody who had waited longer.
  update pd_members set joined_at = now(), queued_at = now()
   where room = p_room and player = v_loser;

  select name into v_name from pd_members where room = p_room and player = v_winner;

  return json_build_object(
    'ok', true,
    'winner', v_winner = v_me,
    'crowned', v_crowned,
    'champion', coalesce(v_name, ''),
    'stocks', p_stocks,
    'beaten', v_beaten,
    'others', v_others);
end $$;

grant execute on function pd_result(text, text, boolean, smallint) to authenticated;
