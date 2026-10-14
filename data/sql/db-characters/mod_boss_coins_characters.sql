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

-- The daily login coins: the day each account last got them, as the time that day ends (the
-- next daily quest reset when they were given). Once per account, not per character: the coins
-- are bound to the account. Rows whose day ended more than a day ago are deleted at startup.
CREATE TABLE IF NOT EXISTS `mod_boss_coins_daily` (
  `account` INT UNSIGNED NOT NULL COMMENT 'account id',
  `period_end` BIGINT UNSIGNED NOT NULL COMMENT 'unix time the day of the last daily coins ends',
  PRIMARY KEY (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: last daily login coins per account';

-- The account wallet: coins that belong to an account rather than to a character's bags. The
-- daily login coins go in, Pix Xizzix moves coins between bags and wallet, and the launcher's
-- shop spends from it. The server keeps the balances in memory, reads this table at startup
-- and changes `coins` by each difference (so it is signed: a change applied ahead of an earlier
-- one never fails).
CREATE TABLE IF NOT EXISTS `mod_boss_coins_wallet` (
  `account` INT UNSIGNED NOT NULL COMMENT 'account id',
  `coins` INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`account`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: account wallet balances';

-- Every change to a wallet: when, which account, the character it came from or went to (0 for
-- none), by how much, the balance after it, and why (daily, deposit, withdraw, shop, gm), with
-- the shop offer for a purchase. Nothing reads it but people: it is the record to settle "where
-- did my coins go".
CREATE TABLE IF NOT EXISTS `mod_boss_coins_ledger` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `at` BIGINT UNSIGNED NOT NULL COMMENT 'unix time',
  `account` INT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'character, 0 for none',
  `coins` INT NOT NULL COMMENT 'the change: more than 0 in, less than 0 out',
  `balance` INT UNSIGNED NOT NULL COMMENT 'the wallet after the change',
  `reason` VARCHAR(16) NOT NULL,
  `offer` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'mod_boss_coins_shop id of a purchase',
  PRIMARY KEY (`id`),
  KEY `account` (`account`, `at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: account wallet changes';
