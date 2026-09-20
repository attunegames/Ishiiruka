-- Rooms, part 7: a room decides whether its matches are drafted.
--
-- A room's stage is RANDOM by default. The draft screen - the bans, then the
-- stage - is off unless the person who owns the room turns it on.
--
-- Why the default is random: that screen is GameSetup.dat, a shipped binary
-- with no source, and its six stage icons are its own. Dolphin does not send it
-- a stage list, so a "random" option cannot be added to it. What Dolphin can do
-- is pick a random legal stage itself, which it already does for every pairing
-- before the draft ever runs - so the honest version of "random stage" is to
-- skip the screen, not to add a button to it.
--
-- ⚠️ THE OWNER ONLY. Everyone in the room can see the setting; one person sets
-- it. Anyone being able to flip it mid-rotation would change the rules out from
-- under a match that is already being set up.
--
-- ⚠️ The owner is whoever owns the room NOW, not whoever made it. Part 5 hands
-- the room to the longest-standing member when the owner leaves, and this reads
-- pd_rooms.owner, so the toggle follows the room rather than the person.

alter table pd_rooms
  add column if not exists stage_draft boolean not null default false;


-- Turn the draft on or off. Returns the setting as it now stands, so a caller
-- that was not allowed to change it still learns what the truth is rather than
-- being left to assume its own request took.
create or replace function pd_set_stage_draft(p_room text, p_on boolean)
returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_me uuid := auth.uid();
  v_on boolean;
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  update pd_rooms
     set stage_draft = p_on
   where code = p_room
     and owner = v_me;

  select stage_draft into v_on from pd_rooms where code = p_room;
  if v_on is null then
    return json_build_object('ok', false, 'error', 'no such room');
  end if;

  return json_build_object('ok', true, 'stageDraft', v_on);
end;
$fn$;

grant execute on function pd_set_stage_draft(text, boolean) to authenticated;
