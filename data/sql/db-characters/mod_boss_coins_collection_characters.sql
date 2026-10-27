-- mod-boss-coins: the achievement mounts and companions each account has been given
-- (src/Collection.cpp). One is given once per account, to the first character that earns the
-- achievement; this table remembers it. Deleting characters doesn't touch it, so a new character
-- earning the achievement again gets nothing. Idempotent.

CREATE TABLE IF NOT EXISTS `mod_boss_coins_collection_achievement` (
  `account` INT UNSIGNED NOT NULL COMMENT 'account id',
  `achievement` INT UNSIGNED NOT NULL COMMENT 'Achievement.dbc id',
  `item` INT UNSIGNED NOT NULL COMMENT 'the item its reward was',
  `guid` INT UNSIGNED NOT NULL COMMENT 'the character that earned it first',
  `at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`account`, `achievement`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: achievement mounts and companions given per account';
