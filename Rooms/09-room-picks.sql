-- Characters are picked in the ROOM, one player at a time, instead of in the
-- draft.
--
-- The winner of the last game picks first, then the challenger. Everybody in
-- the room watches the two boxes fill in. The band across the top still waits
-- for the STAGE and so still rebuilds exactly once, at the splash - which is
-- what keeps this clear of the rebuild freeze of 2026-09-18, whose guard was
-- "only a COMPLETE draft" for exactly this reason.
--
-- ⚠️ pd_tick is PATCHED here, not rewritten. Later migrations have redefined
-- it (stageDraft, isOwner) and a copy taken from an earlier repo .sql silently
-- reverts them. This reads the DEPLOYED definition, substitutes into it, and
-- RAISEs if any substitution matches other than exactly once - so a miss is a
-- failed migration rather than a quietly wrong function.

-- ---------------------------------------------------------------- columns --

-- When the CURRENT picker's clock runs out. One column, not two, because only
-- one person is ever on the clock.
alter table pd_pairings add column if not exists pick_at timestamptz;

-- They asked for a random character. The real roll goes into char_host /
-- char_guest like any other pick - this only says it must stay hidden until the
-- match actually starts.
--
-- ⚠️ The ROLL is stored, not a sentinel. Both clients have to end up playing
-- the same fighter, so exactly one place may roll it and that place is here. A
-- client rolling its own would have the two sides disagree, which is a desync
-- rather than a cosmetic difference.
alter table pd_pairings add column if not exists rand_host  boolean not null default false;
alter table pd_pairings add column if not exists rand_guest boolean not null default false;

-- How long each player gets.
create or replace function pd_pick_window() returns interval
  language sql immutable as $$ select interval '30 seconds' $$;

-- Melee's external character ids run 0..25. Anything outside that is not a
-- fighter, and this is the value the room screen reads as "the question mark".
--
-- ⚠️ Distinct from NOT_PICKED (255). "Has not chosen yet" and "chose the
-- question mark" are different states and the box draws them differently.
create or replace function pd_char_random() returns smallint
  language sql immutable as $$ select 254::smallint $$;

create or replace function pd_roll_char() returns smallint
  language sql volatile as $$ select floor(random() * 26)::smallint $$;


-- ----------------------------------------------------------------- pd_tick --

do $do$
declare
  src  text;
  want text;
  repl text;
  n    int;
  i    int;
  subs text[][];
begin
  select pg_get_functiondef(p.oid) into src
    from pg_proc p join pg_namespace ns on ns.oid = p.pronamespace
   where p.proname = 'pd_tick' and ns.nspname = 'public';

  if src is null then
    raise exception 'pd_tick does not exist - nothing to patch';
  end if;

  -- ⚠️ Mixed line endings. The original body is CRLF and later migrations have
  -- spliced LF-only blocks into it, so a search string written either way
  -- matches in some places and not others. Normalising first costs nothing -
  -- Postgres does not care which the body uses - and makes every match below
  -- deterministic instead of depending on which migration wrote that line.
  src := replace(src, E'\r\n', E'\n');

  subs := array[
    -- 1. A new pairing starts the winner's clock.
    [
$s$      insert into pd_pairings (room, match_id, host, guest)
      values (p_room, 'pd-' || replace(gen_random_uuid()::text, '-', ''),
              v_first, v_second)
      returning * into v_pair;$s$,

$s$      insert into pd_pairings (room, match_id, host, guest, pick_at)
      values (p_room, 'pd-' || replace(gen_random_uuid()::text, '-', ''),
              v_first, v_second, now() + pd_pick_window())
      returning * into v_pair;$s$
    ],

    -- 2. Picking is TAKING A TURN now, so turn-gating replaces the free-for-all.
    [
$s$    if p_char is not null and v_pair.host = v_me then
      update pd_pairings set char_host = p_char, color_host = coalesce(p_color, 0)
       where id = v_pair.id;
    elsif p_char is not null and v_pair.guest = v_me then
      update pd_pairings set char_guest = p_char, color_guest = coalesce(p_color, 0)
       where id = v_pair.id;
    end if;$s$,

$s$    -- The winner of the last game picks first, then the challenger. A pick
    -- arriving OUT OF TURN is dropped rather than applied late: the room only
    -- offers the box to whoever's turn it is, so one that reaches here anyway
    -- is a stale tick from the turn before, and applying it would overwrite a
    -- choice that has already been made and shown to the room.
    --
    -- ⚠️ Whose turn it is is DERIVED from the two character columns, never
    -- stored. A stored turn is a second source of truth that can disagree with
    -- the picks themselves, and that disagreement would be invisible.
    if p_char is not null and v_pair.host = v_me and v_pair.char_host is null then
      update pd_pairings
         set char_host  = case when p_char = pd_char_random()
                               then pd_roll_char() else p_char end,
             rand_host  = (p_char = pd_char_random()),
             color_host = coalesce(p_color, 0),
             -- The challenger's clock starts the moment the winner locks in.
             pick_at    = now() + pd_pick_window()
       where id = v_pair.id;
    elsif p_char is not null and v_pair.guest = v_me
      and v_pair.char_host is not null and v_pair.char_guest is null then
      update pd_pairings
         set char_guest  = case when p_char = pd_char_random()
                                then pd_roll_char() else p_char end,
             rand_guest  = (p_char = pd_char_random()),
             color_guest = coalesce(p_color, 0),
             pick_at     = null   -- both are in; nobody is on the clock
       where id = v_pair.id;
    end if;$s$
    ],

    -- 3. What the room draws, plus whose turn it is and how long is left.
    [
$s$  v_draft := json_build_object(
    'stage',      v_show.stage,
    'hostChar',   v_show.char_host,
    'hostColor',  coalesce(v_show.color_host, 0),
    'guestChar',  v_show.char_guest,
    'guestColor', coalesce(v_show.color_guest, 0),
    'playing',    coalesce(v_show.state = 'ready', false));$s$,

$s$  -- ⚠️ A random pick is MASKED until the match is actually on. The roll is
  -- already sitting in char_host / char_guest - this only decides who may see
  -- it, and until 'ready' the answer is nobody, including the player who asked
  -- for it. That is the whole point of the question mark: the fighter is not
  -- known to either side until the splash screen.
  v_draft := json_build_object(
    'stage',      v_show.stage,
    'hostChar',   case when v_show.rand_host and v_show.state <> 'ready'
                       then pd_char_random() else v_show.char_host end,
    'hostColor',  coalesce(v_show.color_host, 0),
    'guestChar',  case when v_show.rand_guest and v_show.state <> 'ready'
                       then pd_char_random() else v_show.char_guest end,
    'guestColor', coalesce(v_show.color_guest, 0),
    'playing',    coalesce(v_show.state = 'ready', false),
    -- 0 the winner, 1 the challenger, 2 nobody - both are in.
    'pickTurn',   case when v_show.id is null         then 2
                       when v_show.char_host is null  then 0
                       when v_show.char_guest is null then 1
                       else 2 end,
    -- ⚠️ A DEADLINE, given as seconds remaining - not a countdown this
    -- function drives. The room ticks every two seconds and a clock that only
    -- moved that often would be unwatchable; the screen counts down locally
    -- between ticks and this keeps it honest.
    'pickEndsIn', case when v_show.pick_at is null then null
                       else greatest(0, least(255,
                            ceil(extract(epoch from v_show.pick_at - now()))))::int
                  end,
    -- Whether the box on screen belongs to the person reading this.
    'pickIsMine', case when v_show.char_host is null  then v_show.host  = v_me
                       when v_show.char_guest is null then v_show.guest = v_me
                       else false end);$s$
    ],

    -- 4. The clock running out picks for you.
    --
    -- ⚠️ Anchored on the capacity guard's own condition rather than on the
    -- banner above it. The banners are long runs of dashes and getting one
    -- character wrong there fails the migration for no reason; this line is
    -- code, appears exactly once, and cannot drift.
    [
$s$  if not exists (select 1 from pd_members
                  where room = p_room and player = v_me) then$s$,

$s$  -- ----------------------------------------------------------- the clock ---
  --
  -- Nobody chose in time, so the room chooses for them. ROLLED rather than
  -- defaulted to a fighter, and flagged as random, so a timeout looks exactly
  -- like having asked for the question mark - there is no tell that says
  -- somebody ran out of time.
  --
  -- ⚠️ For EVERY pending pairing in the room, not only ours. The player who
  -- ran out of time may be the one whose game has closed, and then their own
  -- tick is never coming - somebody else's has to move it along, or the
  -- pairing sits on an expired clock for ever and holds two people out of the
  -- queue behind it.
  --
  -- ⚠️ The winner's timeout STARTS the challenger's window rather than leaving
  -- pick_at in the past. Without that, the second statement sees a satisfied
  -- condition in the same call and both players are rolled at once.
  update pd_pairings
     set char_host = pd_roll_char(), rand_host = true,
         pick_at   = now() + pd_pick_window()
   where room = p_room and state = 'pending'
     and char_host is null and pick_at is not null and pick_at < now();

  update pd_pairings
     set char_guest = pd_roll_char(), rand_guest = true,
         pick_at    = null
   where room = p_room and state = 'pending'
     and char_host is not null and char_guest is null
     and pick_at is not null and pick_at < now();

  if not exists (select 1 from pd_members
                  where room = p_room and player = v_me) then$s$
    ]
  ];

  for i in 1 .. array_length(subs, 1) loop
    want := subs[i][1];
    repl := subs[i][2];

    -- Exactly once. Zero means the deployed text has moved and this patch is
    -- written against something that is no longer there; more than one means
    -- the anchor is not unique and the wrong copy could be taken.
    n := (length(src) - length(replace(src, want, ''))) / nullif(length(want), 0);
    if coalesce(n, 0) <> 1 then
      raise exception 'pd_tick substitution #% matched % times, wanted 1', i, coalesce(n, 0);
    end if;

    src := replace(src, want, repl);
  end loop;

  execute src;
  raise notice 'pd_tick patched for room character picking';
end $do$;
