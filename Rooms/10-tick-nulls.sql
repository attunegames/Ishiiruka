-- Nothing in a tick reply may be null where the client expects a boolean.
--
-- ⚠️ This is not tidiness. The client reads these with nlohmann's value(),
-- which returns the default for a MISSING key and THROWS for a null one, and
-- the throw is caught around the whole reply - so ONE null boolean discards
-- the ENTIRE tick. Every room screen on three machines went blank for forty
-- minutes on the back of a single one, and the only symptom was a room that
-- quietly did nothing: no queue, no picks, no state at all.
--
-- The three that could be null:
--
--   pickIsMine   with no pairing, v_show.host is null, so "v_show.host = v_me"
--                is NULL rather than false
--   stageDraft   the subquery returns null when there is no pd_rooms row
--   isOwner      the same, and also whenever the owner column itself is null -
--                which happens when the owner leaves
--
-- The client has been hardened too (see Opt() in SlippiRooms.cpp), so neither
-- side relies on the other getting this right. Both, because the server can be
-- fixed without a rebuild and the client protects every field at once.
--
-- ⚠️ Patches the DEPLOYED pd_tick, like 09. Raises if a substitution matches
-- other than exactly once.

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

  src := replace(src, E'\r\n', E'\n');

  subs := array[
    [
$s$    'pickIsMine', case when v_show.char_host is null  then v_show.host  = v_me
                       when v_show.char_guest is null then v_show.guest = v_me
                       else false end);$s$,

$s$    -- coalesced, and that is not belt and braces. A null here is not a
    -- missing field, and the client treats the two differently: value() gives
    -- the default for a MISSING key and THROWS for a null one. The throw takes
    -- the whole tick with it, so one null boolean stopped every room screen
    -- from receiving anything at all.
    'pickIsMine', coalesce(
                    case when v_show.char_host is null  then v_show.host  = v_me
                         when v_show.char_guest is null then v_show.guest = v_me
                         else false end, false));$s$
    ],
    [
$s$    'stageDraft', (select r.stage_draft from pd_rooms r where r.code = p_room),
    'isOwner',    (select r.owner = v_me from pd_rooms r where r.code = p_room),$s$,

$s$    'stageDraft', coalesce((select r.stage_draft from pd_rooms r where r.code = p_room), false),
    'isOwner',    coalesce((select r.owner = v_me from pd_rooms r where r.code = p_room), false),$s$
    ]
  ];

  for i in 1 .. array_length(subs, 1) loop
    want := subs[i][1];
    repl := subs[i][2];
    n := (length(src) - length(replace(src, want, ''))) / nullif(length(want), 0);
    if coalesce(n, 0) <> 1 then
      raise exception 'tick-null substitution #% matched % times, wanted 1', i, coalesce(n, 0);
    end if;
    src := replace(src, want, repl);
  end loop;

  execute src;
  raise notice 'pd_tick returns no nulls where a boolean is expected';
end $do$;
