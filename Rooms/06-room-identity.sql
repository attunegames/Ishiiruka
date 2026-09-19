-- Rooms, part 6: the room says which room it is.
--
-- The screen wants "ROOM QAFK PASS 5143" in the corner and could not have it.
-- pd_tick returns the roster, the queue and the draft, but nothing about the
-- room's own row: the code was known only to whoever typed or created it, and
-- the passcode only to the creator - pd_room_create mints it, returns it once,
-- and Dolphin threw it away.
--
-- ⚠️ Asked ONCE, when the heartbeat starts, rather than folded into pd_tick.
-- Neither value ever changes, and pd_tick already runs twice a second for
-- everyone in every room - putting an unchanging string in it would be paying
-- for it over and over.
--
-- ⚠️ MEMBERS only. The caller has to have a member row in the room it is
-- asking about, so a passcode never leaves the room it belongs to. It
-- deliberately does not go anywhere near pd_room_list, which is the public
-- browser - and a listed room has no passcode in the first place.

create or replace function pd_room_identity(p_room text)
returns json
language sql security definer set search_path = public as $fn$
  select json_build_object(
           'ok',       true,
           'room',     r.code,
           'listed',   r.listed,
           'passcode', coalesce(r.passcode, ''))
    from pd_rooms r
   where r.code = p_room
     and exists (select 1 from pd_members m
                  where m.room = r.code and m.player = auth.uid());
$fn$;

grant execute on function pd_room_identity(text) to authenticated;
