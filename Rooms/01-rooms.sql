-- Rooms rooms, part 1: rooms exist, are listed, and can be joined.
--
-- A room is a place people wait to play each other. Two at a time play, the
-- rest wait in a queue, anyone can watch, and when a match ends the winner
-- stays and the next in line comes in. This file is everything up to "you are
-- in a room"; the queue, the pairing and the rotation are part 2.
--
-- The forked Slippi Dolphin talks to this directly with the token in
-- peppy.json. It replaces the connection to mm.slippi.gg and does the same job
-- Slippi's server does: introduce two clients to each other, then get out of
-- the way. No game traffic passes through here - only addresses and intent.
--
-- Run it once, in the SQL editor, on an empty project.
--
-- Written fresh rather than carried over. The previous version grew a column at
-- a time while the feature was being found, and two of its worst outages came
-- from that shape rather than from any one statement. Where this differs, the
-- comment says why.


-- ============================================================== durations ===
-- How long something stays true without being restated. Functions rather than
-- constants so a running room picks up a change without a redeploy.

create or replace function pd_presence_window() returns interval
  language sql immutable as $fn$ select interval '20 seconds' $fn$;

-- Deliberately much longer than presence: you press Start once and then watch a
-- whole match go by, and that must not drop you out of the queue.
create or replace function pd_queue_window() returns interval
  language sql immutable as $fn$ select interval '150 seconds' $fn$;

-- A NAT mapping is only worth dialling while it is fresh.
create or replace function pd_address_window() returns interval
  language sql immutable as $fn$ select interval '90 seconds' $fn$;


-- ================================================================== rooms ===

create table pd_rooms (
  code        text        primary key,

  -- Only 'singles' is playable. The others are real choices in the menu and
  -- real rows here, so a room can be made and found for them, but nothing
  -- starts a match yet. The constraint is narrow on purpose: an unimplemented
  -- mode should fail here rather than three layers down inside Melee.
  mode        text        not null default 'singles'
                          check (mode in ('singles', 'doubles', 'ironmans',
                                          'crew', 'tournament')),

  -- Public rooms are listed and have no passcode. Private rooms are unlisted
  -- and reachable only by typing the code, so they carry one. One of the two,
  -- never both and never neither.
  listed      boolean     not null,
  passcode    text,
  constraint pd_rooms_passcode_shape check (listed = (passcode is null)),

  owner       uuid        not null,
  owner_name  text        not null,
  created_at  timestamptz not null default now()
);

-- The browser reads this: newest public rooms first, any mode.
create index pd_rooms_browse on pd_rooms (listed, created_at desc);


-- ================================================================ members ===
--
-- Who is in a room, and what each of them is doing.
--
-- THE ONE THING THIS TABLE EXISTS TO GET RIGHT:
--
-- "Are you still here" and "where are you in the queue" are different
-- questions, and the previous version answered both with one column. It was
-- refreshed on every tick, so it meant "last heard from" - and it was also what
-- the queue was ordered by. On your own tick that always sorted you last, so
-- every client saw a different queue and nobody was ever matched. Adding a
-- second column fixed that, and then the rotation broke: the code that sent a
-- loser to the back was still moving the OLD column, which by then was only a
-- tiebreaker, so the same two players re-paired forever while a third waited.
--
-- So here liveness and position are separate columns, and position is written
-- in exactly two places - joining the queue, and being sent to the back. A
-- heartbeat cannot move you in the queue, because the column it touches is not
-- the column the queue reads.

create table pd_members (
  room      text        not null references pd_rooms (code) on delete cascade,
  player    uuid        not null,
  name      text        not null,
  code      text        not null,             -- connect code, e.g. ALPH#694

  -- LIVENESS. Written on every tick. Nothing else may be.
  last_seen timestamptz not null default now(),
  joined_at timestamptz not null default now(),

  -- POSITION. Null means "not in the queue". Set when you press Start, moved
  -- when you are sent to the back, never touched by a heartbeat. This is the
  -- only column the queue is ordered by.
  queued_at timestamptz,

  -- The winner keeps the next game if they want it, and they press Start like
  -- anybody else. Until they do, this holds their place so the player who just
  -- lost cannot press Start first and take it.
  hold_until timestamptz,

  -- ADDRESS. Null most of the time, deliberately: nobody runs STUN while idle.
  -- It happens when a pairing forms, so the mapping is seconds old when the
  -- opponent dials it. pd_address_window decides how long it counts for.
  --
  -- There is no LAN address here. Rooms is for playing people over the
  -- internet, and a LAN shortcut cost every remote watcher a full connect
  -- timeout against a 192.168.x.x that was never theirs. Running several
  -- clients on one machine for testing is a client-side concern, not a column.
  addr       text,                            -- "ip:port" as STUN saw us
  addr_at    timestamptz,

  -- A spectator's own address, so the pair can punch a hole out to them.
  --
  -- There is deliberately no "where I serve a stream from" here. A watcher
  -- attaches to a player's existing netplay socket and is fed the same input
  -- packets the two of them already exchange, so the hole that socket has is
  -- the only one anyone needs.
  watch_addr text,
  watch_at   timestamptz,

  -- Won a clean sweep of the room. Cosmetic, and it sends you to the back.
  crowns     int not null default 0,

  primary key (room, player)
);

create index pd_members_queue on pd_members (room, queued_at) where queued_at is not null;
create index pd_members_live  on pd_members (room, last_seen);


-- ==================================================================== RLS ===
--
-- Closed, with no policies at all. Every read and write goes through a SECURITY
-- DEFINER function below, so the publishable key compiled into the binary
-- grants nothing on its own: it cannot read a room it is not in, or rewrite one
-- it is. This is also why "automatically expose new tables" is off on the
-- project - no table here is meant to be reachable through the Data API.

alter table pd_rooms   enable row level security;
alter table pd_members enable row level security;


-- ============================================================ room codes ===
--
-- Four characters, from an alphabet with no O/0 or I/1 - these get read aloud
-- and typed in by hand.

create or replace function pd_new_code() returns text
language plpgsql security definer set search_path = public as $fn$
declare
  v_alpha constant text := 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  v_code  text;
  v_try   int := 0;
begin
  loop
    v_code := '';
    for i in 1..4 loop
      v_code := v_code || substr(v_alpha, 1 + floor(random() * length(v_alpha))::int, 1);
    end loop;
    exit when not exists (select 1 from pd_rooms where code = v_code);
    v_try := v_try + 1;
    -- 32^4 is a million codes; needing more than a few tries means the table is
    -- full of abandoned rooms, which is a different problem and worth saying.
    if v_try > 20 then
      raise exception 'could not find a free room code after % tries', v_try;
    end if;
  end loop;
  return v_code;
end $fn$;


-- ========================================================== room lifecycle ===

-- Make a room. The caller owns it.
--
-- A passcode is minted here rather than accepted, so a client cannot choose a
-- guessable one, and it is returned to the owner exactly once - it is theirs to
-- pass on however they like.
create or replace function pd_room_create(
  p_mode   text,
  p_listed boolean,
  p_name   text,
  p_code   text
) returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_me   uuid := auth.uid();
  v_room text;
  v_pass text;
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  -- One room at a time. Without this a client that retries a failed create
  -- leaves a trail of empty rooms nobody can see the inside of.
  delete from pd_rooms where owner = v_me;

  v_room := pd_new_code();
  if not p_listed then
    v_pass := lpad(floor(random() * 10000)::text, 4, '0');
  end if;

  insert into pd_rooms (code, mode, listed, passcode, owner, owner_name)
       values (v_room, p_mode, p_listed, v_pass, v_me, p_name);

  -- The owner is in their own room from the moment it exists, so the browser
  -- does not show it with nobody in it.
  insert into pd_members (room, player, name, code)
       values (v_room, v_me, p_name, p_code);

  return json_build_object('ok', true, 'room', v_room, 'mode', p_mode,
                           'listed', p_listed, 'passcode', v_pass);
end $fn$;


-- Every public room, any mode.
--
-- p_mode is a filter, and null means all of them - which is what the menu asks
-- for, because Public is reached before a kind is chosen. Each row says which
-- kind it is so the browser can show that.
create or replace function pd_room_list(p_mode text default null)
returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_rows json;
begin
  select coalesce(json_agg(json_build_object(
           'code',    r.code,
           'mode',    r.mode,
           'owner',   r.owner_name,
           'players', (select count(*) from pd_members m
                        where m.room = r.code
                          and m.last_seen > now() - pd_presence_window())
         ) order by r.created_at desc), '[]'::json)
    into v_rows
    from pd_rooms r
   where r.listed
     and (p_mode is null or r.mode = p_mode)
     -- A room whose last member stopped talking is gone, whatever the table
     -- still says. Showing it sends people somewhere empty.
     and exists (select 1 from pd_members m
                  where m.room = r.code
                    and m.last_seen > now() - pd_presence_window());

  return json_build_object('ok', true, 'rooms', v_rows);
end $fn$;


-- Go into a room. A private one needs its passcode; a public one does not.
create or replace function pd_room_join(
  p_room text,
  p_name text,
  p_code text,
  p_pass text default null
) returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_me   uuid := auth.uid();
  v_room pd_rooms%rowtype;
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  select * into v_room from pd_rooms where code = upper(p_room);
  if v_room.code is null then
    return json_build_object('ok', false, 'error', 'no such room');
  end if;

  -- Checked here rather than in the client, or the passcode is only as good as
  -- the binary asking for it.
  if not v_room.listed and (p_pass is null or p_pass <> v_room.passcode) then
    return json_build_object('ok', false, 'error', 'wrong passcode');
  end if;

  -- Rejoining is not an error - a client that crashed and came back is the
  -- same person in the same room, and must not lose their place in the queue.
  insert into pd_members (room, player, name, code)
       values (v_room.code, v_me, p_name, p_code)
  on conflict (room, player) do update
     set name = excluded.name, code = excluded.code, last_seen = now();

  return json_build_object('ok', true, 'room', v_room.code, 'mode', v_room.mode);
end $fn$;


-- Leave. Closing the room is the owner's to do and takes the members with it,
-- because pd_members references pd_rooms on delete cascade.
create or replace function pd_room_leave(p_room text)
returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_me uuid := auth.uid();
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  delete from pd_members where room = p_room and player = v_me;

  -- The owner walking out closes it. Anyone left is sent back to the menu by
  -- their own next tick finding no room, which is the same path a room that
  -- simply went quiet takes.
  delete from pd_rooms where code = p_room and owner = v_me;

  return json_build_object('ok', true);
end $fn$;


-- ================================================================= grants ===
--
-- Execute only, and only on the functions. No table privileges are granted
-- anywhere, which is what keeps the publishable key harmless.

grant execute on function pd_presence_window()                  to authenticated;
grant execute on function pd_queue_window()                     to authenticated;
grant execute on function pd_address_window()                   to authenticated;
grant execute on function pd_room_create(text, boolean, text, text) to authenticated;
grant execute on function pd_room_list(text)                    to authenticated;
grant execute on function pd_room_join(text, text, text, text)  to authenticated;
grant execute on function pd_room_leave(text)                   to authenticated;
