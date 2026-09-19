-- Rooms, part 5: a room outlives its owner.
--
-- Until now pd_room_leave deleted the room when the owner walked out, and the
-- people still in it were supposed to notice on their next tick. They did not:
-- the room vanished from the public list while somebody sat in it, and their
-- screen carried on showing a roster that no longer existed anywhere.
--
-- So the room is handed on instead. The heir is whoever has been in it LONGEST
-- of those still there, which is the person who joined first after the owner -
-- pd_members.joined_at, which exists and is never rewritten by a heartbeat.
--
-- ⚠️ Only a LIVE member may inherit. joined_at alone would hand the room to
-- whoever joined earliest even if they stopped talking ten minutes ago, and a
-- room owned by a ghost is one nothing can close. Presence is the same window
-- pd_room_list already filters on, so a room that shows in the list has an
-- owner who could have been asked.
--
-- ⚠️ owner_name moves with it. pd_rooms carries a denormalised name and the
-- browser reads THAT, not a join - so a transfer that only moved the uuid would
-- leave the list advertising the host who left.

create or replace function pd_room_leave(p_room text)
returns json
language plpgsql security definer set search_path = public as $fn$
declare
  v_me    uuid := auth.uid();
  v_heir  uuid;
  v_name  text;
begin
  if v_me is null then
    return json_build_object('ok', false, 'error', 'not signed in');
  end if;

  delete from pd_members where room = p_room and player = v_me;

  -- Not the owner: nothing else to settle.
  if not exists (select 1 from pd_rooms where code = p_room and owner = v_me) then
    return json_build_object('ok', true);
  end if;

  -- ⚠️ joined_at then player. Two people can join inside the same millisecond,
  -- and an order that can tie is an order two clients can disagree about.
  select m.player, m.name
    into v_heir, v_name
    from pd_members m
   where m.room = p_room
     and m.last_seen > now() - pd_presence_window()
   order by m.joined_at, m.player
   limit 1;

  if v_heir is null then
    -- Nobody live is left, so there is nothing to hand it to. Members cascade.
    delete from pd_rooms where code = p_room;
    return json_build_object('ok', true, 'closed', true);
  end if;

  update pd_rooms
     set owner = v_heir, owner_name = v_name
   where code = p_room;

  return json_build_object('ok', true, 'owner', v_heir);
end $fn$;

grant execute on function pd_room_leave(text) to authenticated;
