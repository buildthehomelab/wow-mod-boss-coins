-- mod-boss-coins: mounts and companions collected for the account (src/Collection.cpp).
--
--   npc_text 9503210             the box that offers to trade a spare mount or companion
--   mod_boss_coins_exchange      coins for a spare, per item; wins over the
--                                BossCoins.Collection.Exchange.* settings.
--
-- Idempotent. Undo with data/sql/uninstall/mod_boss_coins_uninstall_world.sql.

DELETE FROM `npc_text` WHERE `ID` = 9503210;
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES
(9503210, 'Your account has this one already: every character of yours learns it. Pix Xizzix pays good coin for a spare, and she sends it straight to your account wallet.', 'Your account has this one already: every character of yours learns it. Pix Xizzix pays good coin for a spare, and she sends it straight to your account wallet.', 0, 0, 1);

-- A row per item: `coins` for a spare of it, 0 to never take one. An item without a row is
-- tradeable when it drops and nothing else gives it, at its quality's price. The server reads it
-- at startup.
CREATE TABLE IF NOT EXISTS `mod_boss_coins_exchange` (
  `item` INT UNSIGNED NOT NULL COMMENT 'item_template entry of a mount or companion item',
  `coins` INT UNSIGNED NOT NULL COMMENT 'coins for a spare, 0: not tradeable',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`item`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: coins for a spare mount or companion';

-- Rare quality, but Baron Rivendare's one in a hundred: priced as an epic mount. INSERT IGNORE,
-- so a price you set stays.
INSERT IGNORE INTO `mod_boss_coins_exchange` (`item`, `coins`, `comment`) VALUES
(13335, 25, 'Deathcharger');
