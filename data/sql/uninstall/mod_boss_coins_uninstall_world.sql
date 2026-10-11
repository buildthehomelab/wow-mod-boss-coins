-- mod-boss-coins: undo the module's world database changes. Run it by hand on the world database
-- after removing the module. It isn't applied automatically: AzerothCore only runs the module's
-- db-world and db-characters folders.
--
-- Removes Pix Xizzix with her spawn, greeting, pages and stock, the prices, the coin and the
-- table of coin sources (your own changes to it included). Coins that players already own stay
-- in their bags as items the server no longer knows: see the characters uninstall file. It also
-- forgets that the module's updates were applied, so installing the module again applies them
-- again. Idempotent: safe to run again.

DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 23 AND `SourceGroup` = 9503200;
DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 15 AND `SourceGroup` = 9503200;
DELETE FROM `npc_vendor` WHERE `entry` BETWEEN 9503200 AND 9503299;

DELETE FROM `gossip_menu_option` WHERE `MenuID` = 9503200;
DELETE FROM `gossip_menu` WHERE `MenuID` = 9503200;
DELETE FROM `npc_text` WHERE `ID` = 9503200;

DELETE FROM `creature` WHERE `guid` = 9503200;
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 9503200 AND 9503299;
DELETE FROM `creature_template` WHERE `entry` BETWEEN 9503200 AND 9503299;

DELETE FROM `itemextendedcost_dbc` WHERE `ID` BETWEEN 90411 AND 90429;
DELETE FROM `item_template` WHERE `entry` = 90410;

DROP TABLE IF EXISTS `mod_boss_coins_source`;

DELETE FROM `updates` WHERE `name` LIKE 'mod\_boss\_coins\_%';
