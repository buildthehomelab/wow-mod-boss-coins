-- mod-boss-coins: when each character last got a boss's coins.
--
-- A boss gives a character coins once until the next daily quest reset. The server keeps that in
-- memory; this table is its copy on disk, read at startup so a restart doesn't hand out a second
-- helping. Rows older than two days are deleted at startup. Idempotent.

CREATE TABLE IF NOT EXISTS `mod_boss_coins_award` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'character',
  `encounter` INT UNSIGNED NOT NULL COMMENT 'DungeonEncounter.dbc id of the boss',
  `awarded` BIGINT UNSIGNED NOT NULL COMMENT 'unix time',
  PRIMARY KEY (`guid`, `encounter`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: last coins per character and boss';
