# Changelog

Players on different versions cannot play together (the host refuses the connection). The version is the
network protocol number (`PROTOCOL_VERSION` in `core/Protocol.h`), shown in the Multiplayer window title.
KenshiMP's own history is in [docs/upstream-kenshimp/CHANGELOG.md](docs/upstream-kenshimp/CHANGELOG.md).

## v1022 — first public release

Built on KenshiMP v18. Changes:

- **Sneaking** is shown to other players.
- **Furniture and lights:** towns a client loads while connected keep them (`populateBuilding` is no longer
  blocked on clients; `block_building_populate=1` restores the old behaviour).
- **Town life:** bar patrons and resting townsfolk are mirrored, so NPCs sit as on the host.
- **Doors and gates** follow the host's world: open, closed, locked; a door a client lockpicks is unlocked
  for the host too (`door_sync`).
- **Knock-outs and deaths** of a copy are decided by its owner only: the copy is knocked down, held down,
  woken up, and a death decided in the wrong world is undone.
- **Carrying bodies:** lifting or putting down another player's character (by a player or by the host's
  NPCs) is replayed in the owner's world; a downed copy nobody carries stays where its owner lies.
- **Healing:** first aid and doctoring on another player's character reach the real one.
- **Posture, beds, cages:** crawling, playing dead, in a bed, in a cage, on a prisoner pole travel with the
  character; beds and cages are applied with the engine's own functions.
- **Thefts and crimes:** the crime a client's character commits in the host's world reaches the host's copy,
  in front of the host's NPCs; a theft from a town container is judged by the host from the container's
  owner. Crimes between players are not crimes for the engine (no guards against a player who fights or
  robs another player).
- **Diagnostics:** clients' log lines in the host's log (`share_logs`); host console window outside the game
  (`host_console`, `/console`); RESYNC button and `/resync`.
- **Stability and speed:** engine hooks only record events, the main loop handles them (Kenshi runs damage
  and medicine on its AI worker thread); lighter per-frame work for copies and replicated NPCs.
- Update messages point to the release page; KenshiKin refuses to start next to KenshiMP.
