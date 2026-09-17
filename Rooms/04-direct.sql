-- Rooms, part 4: let Slippi's servers make the connection.
--
-- Everything up to here assumed the room would introduce the two players
-- itself - publish a STUN address each, wait for both to be fresh, then hand
-- them each other. That was the right shape when the goal was to need nothing
-- from slippi.gg. The goal changed: this is meant to BECOME a Slippi feature,
-- so depending on Slippi's own introduction is not a compromise, it is the
-- point.
--
-- So each client now asks Slippi for a DIRECT match against the other's
-- connect code, which pd_members.code has been carrying since part 1, and
-- Slippi does the introduction it already does for every direct match.
--
-- What that deletes: the address gate below, and with it every reason for this
-- project to own STUN, NAT punching or a matchmaking server.
--
-- What it does NOT delete: pd_members.addr and watch_addr. Spectating attaches
-- a watcher to a player's existing netplay socket, and the punch list is how
-- the players learn to open a hole toward them. Same columns, different job.
--
-- ⚠️ The one thing this costs is that a DIRECT match lands on the normal
-- character select rather than ranked's draft. The draft scene is still ours to
-- enter - it reads GamePrepData, which is a struct describing the state of the
-- set, and the room knows every field of it.

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
  v_capacity int;
  v_live    int;
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

  -- ------------------------------------------------------------ capacity ---
  --
  -- ⚠️ Only for somebody who is NOT already in the room. An existing member
  -- ticking has to succeed whatever the count says, or a room that filled up
  -- would start ejecting the very people who filled it.
  --
  -- Counted the same way pd_room_list counts, so the "2/8" the browser shows
  -- and the number this refuses on are the same number. Anything else and a
  -- room reads as having space it will not actually give you.
  --
  -- The race is already handled: the advisory lock at the top of this function
  -- serialises every tick for this room, so two people joining in the same
  -- instant cannot both see the last slot.
  if not exists (select 1 from pd_members
                  where room = p_room and player = v_me) then
    select capacity into v_capacity from pd_rooms where code = p_room;
    select count(*) into v_live from pd_members
     where room = p_room and last_seen > now() - pd_presence_window();

    if v_capacity is not null and v_live >= v_capacity then
      return json_build_object('state', 'full', 'error', 'that room is full',
                               'capacity', v_capacity, 'players', v_live);
    end if;
  end if;

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

  if v_pair.id is null and not p_presence_only then
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

  -- ⚠️ The address gate is GONE, and that is what this migration is for.
  --
  -- The two are introduced by Slippi's own servers now: each client asks for a
  -- DIRECT match against the other's connect code and Slippi does the rest, so
  -- nothing here needs to know where anybody is. Holding a pairing at 'stun'
  -- waiting for addresses nobody collects any more would hold it there for
  -- ever, and the match would simply never start.
  --
  -- pd_members.addr and watch_addr STAY. They are for spectating: a watcher
  -- attaches to a player's existing netplay socket, and the punch list above is
  -- how the two players learn to open a hole toward them. That has nothing to
  -- do with pairing, and dropping the columns would take spectate with it.

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
