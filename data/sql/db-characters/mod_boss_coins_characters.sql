-- mod-boss-coins: the period in which each character last got a boss's coins.
--
-- A boss gives a character coins once per period: a raid boss once per lockout, a dungeon boss
-- once until the next daily quest reset. `period_end` is when that period ends (the raid's next
-- reset, or the next daily quest reset, at the time of the kill). The server keeps this in
-- memory; the table is its copy on disk, read at startup so a restart doesn't hand out a second
-- helping. Rows whose period ended more than a day ago are deleted at startup. Idempotent.

CREATE TABLE IF NOT EXISTS `mod_boss_coins_award` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'character',
  `encounter` INT UNSIGNED NOT NULL COMMENT 'DungeonEncounter.dbc id of the boss',
  `period_end` BIGINT UNSIGNED NOT NULL COMMENT 'unix time the lockout or day of the kill ends',
  PRIMARY KEY (`guid`, `encounter`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: last paid period per character and boss';
