-- Two holes in the rotation, both confirmed against live data.
--
-- 1. THE CHAMPION ONLY TIED WITH THE PERSON THEY BEAT.
--
--    pd_result sends a crowned winner to the back with queued_at = now(), and
--    then sends the loser to the back with queued_at = now(). In PostgreSQL
--    now() is the TRANSACTION's clock, not the statement's, so both writes get
--    the identical timestamp - and joined_at, written the same way, ties too.
--
--    Seen in the table after a crowning in room PX8M:
--
--      Bravo    (loser)     queued_at 18:25:52.021941
--      MrBirdMD (champion)  queued_at 18:25:52.021941   crowns 1
--
--    To the microsecond. The champion was never behind the loser, only level
--    with them, and which of the two got picked first was a coin flip. With
--    three people that is enough to hand the champion the very next game, which
--    is exactly what happened: crowned, then playing again ninety seconds later
--    while the person who had been waiting sat in the queue.
--
-- 2. A 'ready' PAIRING WAS NEVER GIVEN UP ON.
--
--    pd_tick reaps a 'pending' pairing after sixty seconds, and any pairing
--    whose players have left. Nothing reaped a pairing that reached 'ready' -
--    both addresses published, the match on - and then never finished. A draft
--    that deadlocks, a game that disconnects without a result, a Dolphin closed
--    at the wrong moment: the row stays, and pd_tick will not pair either player
--    again while it does, because it excludes anyone already in one.
--
--    Found live: PX8M's newest pairing sat 'ready' with no winner while all
--    three players were still in the room heartbeating. Two of them were locked
--    out and the third had nobody left to pair with, so the room could never
--    arrange another match. Nothing on screen said so.
--
-- ⚠️ THIS PATCHES THE LIVE DEFINITIONS. It does not replace the two functions
-- from a file, and it must not: pd_tick has been redefined by later migrations -
-- it carries stageDraft and isOwner now - so re-applying a copy taken from
-- 02-pairings.sql would silently take those away and break the room's stage
-- toggle. The first draft of this file did exactly that and was caught by
-- reading the deployed function before running it.
--
-- Each replacement is asserted. A missed anchor raises rather than quietly
-- doing nothing, which is the failure mode that matters for a migration whose
-- whole job is a string substitution. Re-running it is harmless: the second run
-- finds the anchors already gone and raises, rather than applying twice.

create or replace function pd_crown_offset() returns interval
  language sql immutable as $fn$ select interval '1 second' $fn$;

grant execute on function pd_crown_offset() to authenticated;

do $mig$
declare
  src     text;
  patched text;
begin
  -- ---------------------------------------------------------------- pd_tick
  select pg_get_functiondef(p.oid) into src
    from pg_proc p join pg_namespace n on n.oid = p.pronamespace
   where n.nspname = 'public' and p.proname = 'pd_tick';

  patched := replace(src,
    'and created_at < now() - interval ''60 seconds'';',
    'and created_at < now() - interval ''60 seconds'';

  update pd_pairings set state = ''done''
   where room = p_room and state = ''ready''
     and created_at < now() - interval ''15 minutes'';');

  if patched = src then
    raise exception 'pd_tick: the pending-reaper anchor was not found';
  end if;
  execute patched;

  -- -------------------------------------------------------------- pd_result
  select pg_get_functiondef(p.oid) into src
    from pg_proc p join pg_namespace n on n.oid = p.pronamespace
   where n.nspname = 'public' and p.proname = 'pd_result';

  patched := replace(src,
    'update pd_members set crowns = crowns + 1, joined_at = now(), queued_at = now()',
    'update pd_members set crowns = crowns + 1, joined_at = now() + pd_crown_offset(), queued_at = now() + pd_crown_offset()');

  if patched = src then
    raise exception 'pd_result: the crown update was not found';
  end if;
  execute patched;
end $mig$;

-- Applied to Slippi-Rooms (aklpyoxkwnzcbtqjjuxk) on 2026-09-20 and verified:
-- pd_tick still has stageDraft and isOwner and now has the fifteen-minute
-- reaper; pd_result calls pd_crown_offset.
--
-- ⚠️ 02-pairings.sql and 01-rooms.sql carry the same changes so a FRESH
-- database gets them from the start. On a fresh install this file is a no-op
-- that raises - which is correct, because there is nothing to patch.
