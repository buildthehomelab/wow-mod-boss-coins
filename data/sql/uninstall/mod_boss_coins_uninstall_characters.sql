-- mod-boss-coins: undo the module's characters database changes. Run it by hand on the characters
-- database after removing the module, with the server stopped.
--
-- Drops the record of who got which boss's coins and which account got its daily coins, and
-- deletes every Tarnished Undermine Real that players own, in bags, bank or mail. Idempotent:
-- safe to run again.

DROP TABLE IF EXISTS `mod_boss_coins_award`;
DROP TABLE IF EXISTS `mod_boss_coins_daily`;

DELETE ci FROM `character_inventory` ci JOIN `item_instance` ii ON ii.`guid` = ci.`item` WHERE ii.`itemEntry` = 90410;
DELETE mi FROM `mail_items` mi JOIN `item_instance` ii ON ii.`guid` = mi.`item_guid` WHERE ii.`itemEntry` = 90410;
DELETE FROM `item_instance` WHERE `itemEntry` = 90410;

DELETE FROM `updates` WHERE `name` LIKE 'mod\_boss\_coins\_%';
