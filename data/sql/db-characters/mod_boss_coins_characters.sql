-- mod-boss-coins: the period in which each character last got a boss's coins.
--
-- A boss gives a character coins once per period: a raid boss once per lockout, a dungeon boss
-- once until the next daily quest reset. `period_end` is when that period ends (the raid's next
-- reset, or the next daily quest reset, at the time of the kill). `instance` is the raid instance
-- the boss died in (0 for a dungeon boss): the same lockout in another instance pays again, which
-- only happens when a game master takes a character off their raid lockout. The server keeps
-- this in memory; the table is its copy on disk, read at startup so a restart doesn't hand out a
-- second helping. Rows whose period ended more than a day ago are deleted at startup. Idempotent.

CREATE TABLE IF NOT EXISTS `mod_boss_coins_award` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'character',
  `encounter` INT UNSIGNED NOT NULL COMMENT 'DungeonEncounter.dbc id of the boss',
  `period_end` BIGINT UNSIGNED NOT NULL COMMENT 'unix time the lockout or day of the kill ends',
  `instance` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'raid instance id of the kill, 0 for a dungeon',
  PRIMARY KEY (`guid`, `encounter`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: last paid period per character and boss';

-- A table made before the `instance` column existed gets it here. MySQL has no ADD COLUMN IF NOT
-- EXISTS, so the statement is only built when the column is missing.
SET @add_instance := (SELECT IF(COUNT(*) = 0,
    'ALTER TABLE `mod_boss_coins_award` ADD COLUMN `instance` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''raid instance id of the kill, 0 for a dungeon'' AFTER `period_end`',
    'DO 0')
  FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_boss_coins_award' AND COLUMN_NAME = 'instance');
PREPARE add_instance FROM @add_instance;
EXECUTE add_instance;
DEALLOCATE PREPARE add_instance;
