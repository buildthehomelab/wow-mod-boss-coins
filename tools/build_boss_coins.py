#!/usr/bin/env python3
"""The data side of the Tarnished Undermine Real: the module's world database SQL, and the rows
the client needs in your realm's spell and item patch.

    python3 build_boss_coins.py --sql ../data/sql/db-world/mod_boss_coins_world.sql
    python3 build_boss_coins.py --patch-w patch-W.MPQ --client /path/to/client --out patch-W-new.MPQ

Both read the same files in data/, so the server and the client can't disagree:

- stock.tsv    what Pix Xizzix sells: page, item, price in coins, and the one class that sees it
               (empty = every class). `name` is for the reader.
- pages.tsv    her pages. A page is a vendor list of its own (a creature entry that is never
               spawned), opened by an option of her gossip menu, because one vendor list holds at
               most 150 items. `achievement` is an achievement the player must have (0 = none):
               the one for killing the raid's last boss, so a page opens for that raid only.
               `phase` is the mod-individual-progression phase a player must have cleared
               (0 = none): the SQL puts a condition on the phase's hidden quest, 66000 + phase.
               Not for a raid that has an achievement: clearing a later phase clears every
               earlier one too. Naxxramas uses it: phase 7 is the last one before The Burning
               Crusade, and only a kill of the 40 player Kel'Thuzad reaches it.
- costs.tsv    the prices: one ItemExtendedCost row per number of coins. Every price in stock.tsv
               needs a row here. Give a new price a new id; never reuse one.
- sources.tsv  who gives coins: a row per instance map, or per boss (`encounter` is the boss's
               DungeonEncounter.dbc id, 0 = every boss of the map; a boss's own row wins).
- credits.tsv  the creatures of a boss that the core's own credit (instance_encounters) doesn't
               fit: it names one of the four that take turns at Zul'Gurub's Edge of Madness, and
               the first death of one of the Temple of Ahn'Qiraj's bug trio. A boss listed here
               is paid by this table alone, when the last creature listed for it in the instance
               dies; list every creature of it, the core's one included. Only creatures spawned
               from the creature table are counted as still alive, not ones a script summons.
               The Four Horsemen of the 40 player Naxxramas are here so that they don't depend
               on that module casting its credit spell.
- shop.tsv     what the launcher's shop sells for coins from the account wallet: an `item`
               (mailed, `count` of it) or a `service` (1 rename, 2 customize, 3 race change,
               4 faction change), never both. An empty `name` is the item's or the service's own.
               `icon` is an Interface/Icons file name for the launcher (empty: the category's).
               Ids below SHOP_OWN_IDS are the module's. Its SQL adds them and keeps what they
               are up to date, but `price`, `sort` and `enabled` only on a new row: a server's
               own prices stay. A `price` of 0 is a free offer: still one per character, and
               not for a character that already has it. Add your own rows to the table from
               SHOP_OWN_IDS on.

--sql writes the world SQL. It needs nothing but the files above.

--patch-w is the patch that carries your realm's Item.dbc and ItemDisplayInfo.dbc (patch-W.MPQ
above is an example name). A patch MPQ replaces whole files, so the new rows go into its
copies; every other file in it is copied unchanged. It gains:

- Item.dbc: the coin, so the client has its icon.
- ItemExtendedCost.dbc: the prices. Without them the vendor window shows no price and the
  client refuses to buy. If the patch has no ItemExtendedCost.dbc yet, the client's own copy
  (--client, newest locale patch) is the base.

Running it again on its own output changes nothing inside (the MPQ's own bytes differ, StormLib
stamps the time). Needs Python 3 and, for the patch, StormLib (`STORMLIB`, default
/usr/local/lib/libstorm.dylib).
"""

import argparse
import csv
import ctypes
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")

COIN = 90410
COIN_NAME = "Tarnished Undermine Real"
COIN_TEXT = "Stamped with the face of a long-forgotten trade prince. Pix Xizzix still takes them."
COIN_DISPLAY = 55288            # ItemDisplayInfo.dbc: a Dalaran fountain copper coin's, icon INV_Misc_Coin_19
COIN_STACK = 250                # one stack; a character can own any number
ITEM_CLASS_MISC, ITEM_SUBCLASS_JUNK, MATERIAL_JEWELRY = 15, 0, 4
QUALITY_COMMON, BIND_ON_PICKUP = 1, 1
ITEM_FLAG_BOUND_TO_ACCOUNT = 0x08000000   # with bind on pickup: can be mailed, but only to the player's own characters
COST_IDS = range(90411, 90430)  # this module's ItemExtendedCost ids
UNSET = 0xFFFFFFFF

# Pix Xizzix. Friendly to everyone, can't be attacked. She stands on the Booty Bay boardwalk,
# where the module's SQL spawns her.
PIX = 9503200                   # her creature entry, spawn guid, gossip menu and greeting text
PIX_NAME, PIX_TITLE = "Pix Xizzix", "Undermine Real Exchange"
PIX_DISPLAY = 10665             # a goblin woman (Styleen Silvercart's, Everlook's exotic item vendor)
PIX_LEVEL = 60
PIX_SPOT = (0, -14339.4, 440.842, 7.46643, 1.39431)   # map, x, y, z, facing
PIX_GREETING = (
    "Looking to trade, $c? I deal in Tarnished Undermine Reals and nothing else.$B$B"
    "The bosses of Blackrock Mountain carry them. So does Onyxia, so do the trolls of Zul'Gurub, "
    "whatever crawls in Ahn'Qiraj and the dead of Naxxramas, and so do the ones in Stratholme, "
    "Scholomance and Dire Maul. A raid boss pays you once each time its raid resets, a dungeon "
    "boss once a day.$B$B"
    "Dungeon sets I sell to anybody. What my crews hauled out of the raids I keep for people who "
    "finish a job: put Ragnaros down and I'll open the Molten Core crates. Onyxia, Nefarian, "
    "Hakkar, Ossirian, C'Thun, Kel'Thuzad: bring down any of them and I'll show you what came out "
    "of their halls.")
ENTRIES = range(9503200, 9503300)   # this module's creature entries
PIX_SCRIPT = "npc_boss_coins_pix"   # adds the account wallet's options to her menu

SHOP_OWN_IDS = 1000             # shop offers below this id are the module's
SHOP_SERVICES = {1: "rename", 2: "customize", 3: "race change", 4: "faction change"}

CLASS_MASKS = {"warrior": 1, "paladin": 2, "hunter": 4, "rogue": 8, "priest": 16, "death knight": 32,
               "shaman": 64, "mage": 128, "warlock": 256, "druid": 1024}
MAX_VENDOR_ITEMS = 150

# mod-individual-progression keeps a player's phase as rewarded hidden quests: 66000 + phase, for
# every phase up to theirs.
PROGRESSION_QUEST = 66000
CONDITION_SOURCE_GOSSIP_MENU_OPTION = 15
CONDITION_SOURCE_NPC_VENDOR = 23
CONDITION_QUESTREWARDED, CONDITION_CLASS, CONDITION_ACHIEVEMENT = 8, 15, 17
NPCFLAG_GOSSIP, NPCFLAG_VENDOR = 1, 128
# The options open a vendor list but show the chat bubble, not the vendor bag: a 3.3.5 client that
# is offered a single option with any other icon picks it without showing the greeting, and the
# greeting is where a new player reads how the other pages open.
GOSSIP_OPTION_VENDOR, GOSSIP_ICON_CHAT = 3, 0

LOCALE_MPQS = ["patch-{0}-3.MPQ", "patch-{0}-2.MPQ", "patch-{0}.MPQ", "locale-{0}.MPQ"]
# ItemExtendedCost.dbc: id, honor, arena points, arena bracket, 5 items, 5 counts, rating, group
COST_FIELDS, COST_ITEM, COST_COUNT = 16, 4, 9
ITEM_FIELDS, ITEM_DISPLAY_FIELD = 8, 5


# --- the data files ----------------------------------------------------------------------------

def read_tsv(name):
    path = os.path.join(DATA, name)
    with open(path, encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))
    if not rows:
        sys.exit("%s is empty" % path)
    return rows


def load():
    costs = {}
    for r in read_tsv("costs.tsv"):
        cost, coins = int(r["cost"]), int(r["coins"])
        if cost not in COST_IDS:
            sys.exit("costs.tsv: %d is outside %d-%d" % (cost, COST_IDS[0], COST_IDS[-1]))
        if coins in costs or cost in costs.values() or coins <= 0:
            sys.exit("costs.tsv: %d coins / id %d is listed twice or can't be paid" % (coins, cost))
        costs[coins] = cost

    pages = {}
    for r in read_tsv("pages.tsv"):
        page = dict(key=r["page"], vendor=int(r["vendor"]), phase=int(r["phase"]),
                    achievement=int(r["achievement"]), option=r["option"],
                    holds=r["holds"], stock=[])
        if page["vendor"] not in ENTRIES or page["vendor"] == PIX:
            sys.exit("pages.tsv: %d is outside this module's entries" % page["vendor"])
        if page["key"] in pages or page["vendor"] in [p["vendor"] for p in pages.values()]:
            sys.exit("pages.tsv: %s is listed twice" % page["key"])
        pages[page["key"]] = page

    seen = set()
    for r in read_tsv("stock.tsv"):
        item, coins = int(r["item"]), int(r["coins"])
        if r["page"] not in pages:
            sys.exit("stock.tsv: no page %s in pages.tsv" % r["page"])
        if coins not in costs:
            sys.exit("stock.tsv: %s costs %d coins, which has no row in costs.tsv" % (r["name"], coins))
        if r["class"] and r["class"] not in CLASS_MASKS:
            sys.exit("stock.tsv: %s: unknown class %s" % (r["name"], r["class"]))
        if item in seen:
            # her conditions are per item, whatever the page, so an item can be sold once
            sys.exit("stock.tsv: item %d is listed twice" % item)
        seen.add(item)
        pages[r["page"]]["stock"].append(dict(item=item, coins=coins, cls=r["class"], name=r["name"]))
    for page in pages.values():
        if not 0 < len(page["stock"]) <= MAX_VENDOR_ITEMS:
            sys.exit("page %s has %d items; a vendor list holds 1 to %d" % (
                page["key"], len(page["stock"]), MAX_VENDOR_ITEMS))

    sources = [dict(map=int(r["map"]), encounter=int(r["encounter"]), coins=int(r["coins"]), comment=r["comment"])
               for r in read_tsv("sources.tsv")]
    credits = [dict(creature=int(r["creature"]), encounter=int(r["encounter"]), comment=r["comment"])
               for r in read_tsv("credits.tsv")]
    if len({c["creature"] for c in credits}) != len(credits):
        sys.exit("credits.tsv: a creature is listed twice")

    shop = []
    for r in read_tsv("shop.tsv"):
        offer = dict(id=int(r["id"]), category=r["category"], item=int(r["item"]), count=int(r["count"]),
                     service=int(r["service"]), price=int(r["price"]), sort=int(r["sort"]), name=r["name"],
                     description=r["description"], icon=r["icon"], comment=r["comment"])
        if not 0 < offer["id"] < SHOP_OWN_IDS or offer["id"] in [o["id"] for o in shop]:
            sys.exit("shop.tsv: id %d is listed twice or isn't below %d" % (offer["id"], SHOP_OWN_IDS))
        if bool(offer["item"]) == bool(offer["service"]) or (offer["service"] and offer["service"] not in SHOP_SERVICES):
            sys.exit("shop.tsv: offer %d needs either an item or a service (1-4)" % offer["id"])
        if offer["price"] < 0 or not 0 < offer["count"] < 256:
            sys.exit("shop.tsv: offer %d needs a price (0 = free) and a count of 1-255" % offer["id"])
        shop.append(offer)
    return costs, list(pages.values()), sources, credits, shop


# --- SQL ---------------------------------------------------------------------------------------

def sql_str(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def id_list(values):
    return ", ".join(str(v) for v in values)


CONDITION_COLUMNS = ("INSERT INTO `conditions` (`SourceTypeOrReferenceId`, `SourceGroup`, `SourceEntry`, `SourceId`, "
                     "`ElseGroup`, `ConditionTypeOrReference`, `ConditionTarget`, `ConditionValue1`, `ConditionValue2`, "
                     "`ConditionValue3`, `NegativeCondition`, `ErrorType`, `ErrorTextId`, `ScriptName`, `Comment`) VALUES\n")


def write_sql(path, costs, pages, sources, credits, shop):
    lists = [p["vendor"] for p in pages]
    with open(path, "w", encoding="utf-8") as f:
        w = f.write
        w("-- mod-boss-coins: the Tarnished Undermine Real, Pix Xizzix who trades for it, and who gives it.\n")
        w("-- Generated by tools/build_boss_coins.py from the files in data/: change those and run the\n")
        w("-- tool again, don't edit this file.\n")
        w("--\n")
        w("--   item %d            Tarnished Undermine Real: binds to account, stacks to %d, no cap.\n" % (COIN, COIN_STACK))
        w("--   itemextendedcost   %d-%d: one row per price. The client has the same rows in its\n" % (
            min(costs.values()), max(costs.values())))
        w("--                      patch; a price the client doesn't know can't be paid.\n")
        w("--   creature %d   %s, in Booty Bay (also her spawn guid, gossip menu and text).\n" % (PIX, PIX_NAME))
        w("--   creature %d-%d  her pages: vendor lists, never spawned.\n" % (min(lists), max(lists)))
        w("--   mod_boss_coins_source  which bosses give coins, and how many.\n")
        w("--   mod_boss_coins_credit  creatures whose death also counts as one of those bosses.\n")
        w("--   mod_boss_coins_shop    what the launcher's shop sells for wallet coins.\n")
        w("--\n")
        w("-- This module's ids: item %d and ItemExtendedCost %d-%d; creature entries, the spawn\n" % (
            COIN, COST_IDS[0], COST_IDS[-1]))
        w("-- guid, gossip menu and npc_text %d-%d. No stock rows are changed. Idempotent.\n" % (ENTRIES[0], ENTRIES[-1]))
        w("-- Undo with data/sql/uninstall/mod_boss_coins_uninstall_world.sql.\n\n")

        w("-- The coin. Junk to a vendor (no sell price), so it can only be spent. Bound to the account:\n")
        w("-- a player can mail it to their other characters, and to nobody else.\n")
        w("DELETE FROM `item_template` WHERE `entry` = %d;\n" % COIN)
        w("INSERT INTO `item_template` (`entry`, `class`, `subclass`, `name`, `displayid`, `Quality`, `Flags`, `BuyPrice`, `SellPrice`, `ItemLevel`, `maxcount`, `stackable`, `bonding`, `description`, `Material`) VALUES\n")
        w("(%d, %d, %d, %s, %d, %d, %d, 0, 0, 1, %d, %d, %d, %s, %d);\n\n" % (
            COIN, ITEM_CLASS_MISC, ITEM_SUBCLASS_JUNK, sql_str(COIN_NAME), COIN_DISPLAY, QUALITY_COMMON,
            ITEM_FLAG_BOUND_TO_ACCOUNT, 0, COIN_STACK, BIND_ON_PICKUP, sql_str(COIN_TEXT), MATERIAL_JEWELRY))

        w("-- The prices, in coins.\n")
        w("DELETE FROM `itemextendedcost_dbc` WHERE `ID` BETWEEN %d AND %d;\n" % (COST_IDS[0], COST_IDS[-1]))
        w("INSERT INTO `itemextendedcost_dbc` (`ID`, `HonorPoints`, `ArenaPoints`, `ArenaBracket`, `ItemID_1`, `ItemID_2`, `ItemID_3`, `ItemID_4`, `ItemID_5`, `ItemCount_1`, `ItemCount_2`, `ItemCount_3`, `ItemCount_4`, `ItemCount_5`, `RequiredArenaRating`, `ItemPurchaseGroup`) VALUES\n")
        w(",\n".join("(%d, 0, 0, 0, %d, 0, 0, 0, 0, %d, 0, 0, 0, 0, 0, 0)" % (cost, COIN, coins)
                     for coins, cost in sorted(costs.items())) + ";\n\n")

        w("-- %s and her pages. The pages are creature entries only so that they can own a vendor\n" % PIX_NAME)
        w("-- list; nothing spawns them.\n")
        w("DELETE FROM `creature` WHERE `guid` = %d;\n" % PIX)
        w("DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN %d AND %d;\n" % (ENTRIES[0], ENTRIES[-1]))
        w("DELETE FROM `creature_template` WHERE `entry` BETWEEN %d AND %d;\n" % (ENTRIES[0], ENTRIES[-1]))
        w("INSERT INTO `creature_template` (`entry`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `BaseAttackTime`, `RangeAttackTime`, `unit_class`, `unit_flags`, `unit_flags2`, `type`, `flags_extra`, `ScriptName`) VALUES\n")
        rows = ["(%d, %s, %s, %d, %d, %d, 35, %d, 1, 1.14286, 2000, 2000, 1, 768, 2048, 7, 2, %s)" % (
            PIX, sql_str(PIX_NAME), sql_str(PIX_TITLE), PIX, PIX_LEVEL, PIX_LEVEL, NPCFLAG_GOSSIP | NPCFLAG_VENDOR, sql_str(PIX_SCRIPT))]
        rows += ["(%d, %s, %s, 0, %d, %d, 35, %d, 1, 1.14286, 2000, 2000, 1, 768, 2048, 7, 2, '')" % (
            p["vendor"], sql_str(PIX_NAME), sql_str("page: " + p["holds"]), PIX_LEVEL, PIX_LEVEL, NPCFLAG_VENDOR)
            for p in pages]
        w(",\n".join(rows) + ";\n")
        w("INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`) VALUES\n")
        w(",\n".join("(%d, 0, %d, 1, 1)" % (entry, PIX_DISPLAY) for entry in [PIX] + lists) + ";\n")
        w("INSERT INTO `creature` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `MovementType`, `Comment`) VALUES\n")
        w("(%d, %d, %d, 1, 1, %g, %g, %g, %g, 300, 0, 0, %s);\n\n" % (
            PIX, PIX, *PIX_SPOT, sql_str("mod-boss-coins: %s, Booty Bay boardwalk" % PIX_NAME)))

        w("-- Her greeting, and an option per page. An option opens the page's vendor list (the core\n")
        w("-- takes the list from ActionMenuID). A page with a phase is only offered to a player who\n")
        w("-- has cleared that mod-individual-progression phase, a page with an achievement to a\n")
        w("-- player who has it.\n")
        w("DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = %d AND `SourceGroup` = %d;\n" % (
            CONDITION_SOURCE_GOSSIP_MENU_OPTION, PIX))
        w("DELETE FROM `gossip_menu_option` WHERE `MenuID` = %d;\n" % PIX)
        w("DELETE FROM `gossip_menu` WHERE `MenuID` = %d;\n" % PIX)
        w("DELETE FROM `npc_text` WHERE `ID` = %d;\n" % PIX)
        w("INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `BroadcastTextID0`, `lang0`, `Probability0`) VALUES\n")
        w("(%d, %s, %s, 0, 0, 1);\n" % (PIX, sql_str(PIX_GREETING), sql_str(PIX_GREETING)))
        w("INSERT INTO `gossip_menu` (`MenuID`, `TextID`) VALUES\n(%d, %d);\n" % (PIX, PIX))
        w("INSERT INTO `gossip_menu_option` (`MenuID`, `OptionID`, `OptionIcon`, `OptionText`, `OptionBroadcastTextID`, `OptionType`, `OptionNpcFlag`, `ActionMenuID`, `ActionPoiID`, `BoxCoded`, `BoxMoney`, `BoxText`, `BoxBroadcastTextID`) VALUES\n")
        w(",\n".join("(%d, %d, %d, %s, 0, %d, %d, %d, 0, 0, 0, '', 0)" % (
            PIX, n, GOSSIP_ICON_CHAT, sql_str(p["option"]), GOSSIP_OPTION_VENDOR, NPCFLAG_VENDOR, p["vendor"])
            for n, p in enumerate(pages)) + ";\n")
        gates = []
        for n, p in enumerate(pages):
            if p["phase"]:
                gates.append((n, CONDITION_QUESTREWARDED, PROGRESSION_QUEST + p["phase"],
                              "%s shows %s to a player who has cleared progression phase %d" % (PIX_NAME, p["holds"], p["phase"])))
            if p["achievement"]:
                gates.append((n, CONDITION_ACHIEVEMENT, p["achievement"],
                              "%s shows %s to a player who has achievement %d" % (PIX_NAME, p["holds"], p["achievement"])))
        if gates:
            w(CONDITION_COLUMNS)
            w(",\n".join("(%d, %d, %d, 0, 0, %d, 0, %d, 0, 0, 0, 0, 0, '', %s)" % (
                CONDITION_SOURCE_GOSSIP_MENU_OPTION, PIX, n, kind, value, sql_str(comment))
                for n, kind, value, comment in gates) + ";\n")
        w("\n")

        w("-- What she sells. A class's pieces are shown and sold to that class only; the condition\n")
        w("-- is on her own entry (the core asks the creature the player talks to, not the page).\n")
        w("DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = %d AND `SourceGroup` = %d;\n" % (
            CONDITION_SOURCE_NPC_VENDOR, PIX))
        w("DELETE FROM `npc_vendor` WHERE `entry` BETWEEN %d AND %d;\n" % (ENTRIES[0], ENTRIES[-1]))
        for p in pages:
            w("-- %s\n" % p["holds"])
            w("INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`) VALUES\n")
            w(",\n".join("(%d, %d, %d, 0, 0, %d)" % (p["vendor"], slot, s["item"], costs[s["coins"]])
                         for slot, s in enumerate(p["stock"], 1)) + ";\n")
        classed = [s for p in pages for s in p["stock"] if s["cls"]]
        if classed:
            w(CONDITION_COLUMNS)
            w(",\n".join("(%d, %d, %d, 0, 0, %d, 0, %d, 0, 0, 0, 0, 0, '', %s)" % (
                CONDITION_SOURCE_NPC_VENDOR, PIX, s["item"], CONDITION_CLASS, CLASS_MASKS[s["cls"]],
                sql_str("%s sells %s to a %s" % (PIX_NAME, s["name"], s["cls"]))) for s in classed) + ";\n")
        w("\n")

        w("-- Who gives coins. `map` is the instance; `encounter` is a boss's DungeonEncounter.dbc id,\n")
        w("-- or 0 for every boss of the map that has no row of its own. `coins` 0 switches a row off:\n")
        w("-- on a boss's own row, that one boss.\n")
        w("-- The rows below are only added when missing, so your own changes to them stay; the\n")
        w("-- server reads the table at startup.\n")
        w("CREATE TABLE IF NOT EXISTS `mod_boss_coins_source` (\n")
        w("  `map` SMALLINT UNSIGNED NOT NULL,\n")
        w("  `encounter` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'DungeonEncounter.dbc id; 0 = every boss of the map',\n")
        w("  `coins` TINYINT UNSIGNED NOT NULL DEFAULT 1,\n")
        w("  `comment` VARCHAR(255) NOT NULL DEFAULT '',\n")
        w("  PRIMARY KEY (`map`, `encounter`)\n")
        w(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: bosses that give coins';\n")
        w("INSERT IGNORE INTO `mod_boss_coins_source` (`map`, `encounter`, `coins`, `comment`) VALUES\n")
        w(",\n".join("(%d, %d, %d, %s)" % (s["map"], s["encounter"], s["coins"], sql_str(s["comment"]))
                     for s in sources) + ";\n\n")

        w("-- The creatures of bosses that the core's own credit doesn't fit. `encounter` is the boss's\n")
        w("-- DungeonEncounter.dbc id. A boss with rows here is paid by them alone, when the last of its\n")
        w("-- creatures in the instance dies (of those spawned from the creature table: summoned ones\n")
        w("-- aren't seen), so every creature of it has to be listed. Added when missing and read at\n")
        w("-- startup, like the table above.\n")
        w("CREATE TABLE IF NOT EXISTS `mod_boss_coins_credit` (\n")
        w("  `creature` INT UNSIGNED NOT NULL COMMENT 'creature_template entry',\n")
        w("  `encounter` INT UNSIGNED NOT NULL COMMENT 'DungeonEncounter.dbc id of the boss it is (part of)',\n")
        w("  `comment` VARCHAR(255) NOT NULL DEFAULT '',\n")
        w("  PRIMARY KEY (`creature`)\n")
        w(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: the creatures of bosses paid on their last death';\n")
        w("INSERT IGNORE INTO `mod_boss_coins_credit` (`creature`, `encounter`, `comment`) VALUES\n")
        w(",\n".join("(%d, %d, %s)" % (c["creature"], c["encounter"], sql_str(c["comment"])) for c in credits) + ";\n\n")

        w("-- What the launcher's shop sells, for coins from the account wallet. An offer is an item,\n")
        w("-- mailed to the character it is bought for, or a service the character gets at the\n")
        w("-- character list (`service` 1 rename, 2 customize, 3 race change, 4 faction change). An\n")
        w("-- offer whose item isn't on the server is left out. Ids below %d are this module's: what\n" % SHOP_OWN_IDS)
        w("-- they are follows the module, but `price`, `sort` and `enabled` are only set when an offer\n")
        w("-- is new, so prices you change stay. An offer the module no longer has is removed. Add your\n")
        w("-- own offers from %d on, they stay. The server reads the table at startup and on\n" % SHOP_OWN_IDS)
        w("-- .coinshop reload.\n")
        w("CREATE TABLE IF NOT EXISTS `mod_boss_coins_shop` (\n")
        w("  `id` INT UNSIGNED NOT NULL,\n")
        w("  `category` VARCHAR(16) NOT NULL DEFAULT '' COMMENT 'the launcher groups offers by it: mount, pet, service, ...',\n")
        w("  `name` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'empty: the item\\'s or the service\\'s own name',\n")
        w("  `description` VARCHAR(255) NOT NULL DEFAULT '',\n")
        w("  `icon` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'Interface/Icons file name for the launcher; empty: the category\\'s',\n")
        w("  `price` INT UNSIGNED NOT NULL COMMENT 'coins',\n")
        w("  `item` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'item_template entry, mailed',\n")
        w("  `count` TINYINT UNSIGNED NOT NULL DEFAULT 1,\n")
        w("  `service` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 rename, 2 customize, 3 race change, 4 faction change',\n")
        w("  `sort` INT NOT NULL DEFAULT 0,\n")
        w("  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,\n")
        w("  `comment` VARCHAR(255) NOT NULL DEFAULT '',\n")
        w("  PRIMARY KEY (`id`)\n")
        w(") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-boss-coins: the launcher shop';\n")
        w("DELETE FROM `mod_boss_coins_shop` WHERE `id` < %d AND `id` NOT IN (%s);\n" % (
            SHOP_OWN_IDS, id_list(o["id"] for o in shop)))
        w("INSERT INTO `mod_boss_coins_shop` (`id`, `category`, `name`, `description`, `icon`, `price`, `item`, `count`, `service`, `sort`, `enabled`, `comment`) VALUES\n")
        w(",\n".join("(%d, %s, %s, %s, %s, %d, %d, %d, %d, %d, 1, %s)" % (
            o["id"], sql_str(o["category"]), sql_str(o["name"]), sql_str(o["description"]), sql_str(o["icon"]),
            o["price"], o["item"], o["count"], o["service"], o["sort"], sql_str(o["comment"])) for o in shop) + "\n")
        w("ON DUPLICATE KEY UPDATE `category` = VALUES(`category`), `name` = VALUES(`name`), `description` = VALUES(`description`), "
          "`icon` = VALUES(`icon`), `item` = VALUES(`item`), `count` = VALUES(`count`), `service` = VALUES(`service`), "
          "`comment` = VALUES(`comment`);\n")
    print("wrote %s: %d items on %d pages, %d prices, %d coin sources, %d stand-in bosses, %d shop offers" % (
        path, sum(len(p["stock"]) for p in pages), len(pages), len(costs), len(sources), len(credits), len(shop)))


# --- DBC ---------------------------------------------------------------------------------------

def read_dbc(path):
    data = open(path, "rb").read()
    magic, count, fields, size, strsize = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC" or size != fields * 4:
        sys.exit("%s isn't a plain WDBC file" % path)
    rows = [list(struct.unpack_from("<%dI" % fields, data, 20 + i * size)) for i in range(count)]
    strings = bytearray(data[20 + count * size:20 + count * size + strsize])
    return rows, strings, fields


def write_dbc(path, rows, strings, fields):
    rows = sorted(rows, key=lambda r: r[0])
    with open(path, "wb") as f:
        f.write(struct.pack("<4s4I", b"WDBC", len(rows), fields, fields * 4, len(strings)))
        for r in rows:
            f.write(struct.pack("<%dI" % fields, *r))
        f.write(strings)


def upsert(rows, row):
    rows[:] = [r for r in rows if r[0] != row[0]]
    rows.append(row)


def claim(rows, row_id, table, ours):
    """Stops if the patch already uses one of our ids for something else."""
    old = next((r for r in rows if r[0] == row_id), None)
    if old and not ours(old):
        sys.exit("%s.dbc row %d is taken by something else" % (table, row_id))


# --- MPQ ---------------------------------------------------------------------------------------

def stormlib():
    lib = ctypes.CDLL(os.environ.get("STORMLIB", "/usr/local/lib/libstorm.dylib"))
    H = ctypes.c_void_p
    lib.SFileOpenArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]
    lib.SFileExtractFile.argtypes = [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    lib.SFileFindFirstFile.argtypes = [H, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_char_p]
    lib.SFileFindFirstFile.restype = H
    lib.SFileFindNextFile.argtypes = [H, ctypes.c_void_p]
    lib.SFileFindClose.argtypes = [H]
    lib.SFileCreateArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(H)]
    lib.SFileAddFileEx.argtypes = [H, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
    lib.SFileCloseArchive.argtypes = [H]
    for fn in (lib.SFileOpenArchive, lib.SFileExtractFile, lib.SFileFindNextFile, lib.SFileFindClose,
               lib.SFileCreateArchive, lib.SFileAddFileEx, lib.SFileCloseArchive):
        fn.restype = ctypes.c_bool
    return lib, H


def open_mpq(lib, H, mpq):
    h = H()
    if not lib.SFileOpenArchive(mpq.encode(), 0, 0x100, ctypes.byref(h)):
        sys.exit("can't open " + mpq)
    return h


def unpack(mpq, folder):
    """Extracts every file; returns {archive name: local path}."""
    lib, H = stormlib()
    h = open_mpq(lib, H, mpq)
    names = []
    found = ctypes.create_string_buffer(2048)  # SFILE_FIND_DATA starts with the name (1024 bytes)
    find = lib.SFileFindFirstFile(h, b"*", found, None)
    while find:
        name = found.value.decode("latin-1")
        if name not in ("(listfile)", "(attributes)", "(signature)"):
            names.append(name)
        if not lib.SFileFindNextFile(find, found):
            lib.SFileFindClose(find)
            break
    files = {}
    for i, name in enumerate(names):
        local = os.path.join(folder, "%05d" % i)
        if not lib.SFileExtractFile(h, name.encode("latin-1"), local.encode(), 0):
            sys.exit("can't extract %s from %s" % (name, mpq))
        files[name] = local
    lib.SFileCloseArchive(h)
    if not files:
        sys.exit("%s has no files (or no listfile)" % mpq)
    return files


def client_file(client, locale, name, dst):
    """The client's newest copy of a locale file (the later locale patches load over the earlier)."""
    lib, H = stormlib()
    folder = os.path.join(client, "Data", locale)
    for pattern in LOCALE_MPQS:
        mpq = os.path.join(folder, pattern.format(locale))
        if not os.path.exists(mpq):
            continue
        h = open_mpq(lib, H, mpq)
        ok = lib.SFileExtractFile(h, name.encode("latin-1"), dst.encode(), 0)
        lib.SFileCloseArchive(h)
        if ok:
            return mpq
    sys.exit("no %s in %s's locale MPQs" % (name, folder))


def pack(out, files):
    """files: [(local path, archive name)]. zlib-compressed, with listfile and attributes."""
    lib, H = stormlib()
    if os.path.exists(out):
        os.remove(out)
    h = H()
    if not lib.SFileCreateArchive(out.encode(), 0x00300000, max(64, len(files) * 2), ctypes.byref(h)):
        sys.exit("can't create " + out)
    for src, name in files:
        if not lib.SFileAddFileEx(h, src.encode(), name.encode("latin-1"), 0x80000200, 0x02, 0x02):
            lib.SFileCloseArchive(h)
            os.remove(out)
            sys.exit("can't add " + name)
    if not lib.SFileCloseArchive(h):
        os.remove(out)
        sys.exit("can't finish " + out)


# --- the patch ---------------------------------------------------------------------------------

def patch_w(files, costs, args, tmp):
    """Adds the coin and the prices to the unpacked patch."""
    def dbc(table):
        return "DBFilesClient\\%s.dbc" % table

    for table in ("Item", "ItemDisplayInfo"):
        if dbc(table) not in files:
            sys.exit("%s has no %s.dbc" % (args.patch_w, table))
    if dbc("ItemExtendedCost") not in files:
        if not args.client:
            sys.exit("%s has no ItemExtendedCost.dbc yet: give --client to start from the client's" % args.patch_w)
        local = os.path.join(tmp, "ItemExtendedCost.dbc")
        source = client_file(args.client, args.locale, dbc("ItemExtendedCost"), local)
        print("ItemExtendedCost.dbc: new to the patch, from", os.path.basename(source))
        files[dbc("ItemExtendedCost")] = local

    items, item_str, item_fields = read_dbc(files[dbc("Item")])
    displays, _, _ = read_dbc(files[dbc("ItemDisplayInfo")])
    prices, price_str, price_fields = read_dbc(files[dbc("ItemExtendedCost")])
    if item_fields != ITEM_FIELDS or price_fields != COST_FIELDS:
        sys.exit("Item.dbc or ItemExtendedCost.dbc isn't the 3.3.5a layout")
    if not any(r[0] == COIN_DISPLAY for r in displays):
        sys.exit("ItemDisplayInfo.dbc has no row %d for the coin's icon" % COIN_DISPLAY)

    claim(items, COIN, "Item", lambda r: r[ITEM_DISPLAY_FIELD] == COIN_DISPLAY)
    for cost in COST_IDS:
        claim(prices, cost, "ItemExtendedCost", lambda r: r[COST_ITEM] == COIN)
    # ours, and nothing of ours that costs.tsv no longer lists
    prices[:] = [r for r in prices if r[0] not in COST_IDS]

    upsert(items, [COIN, ITEM_CLASS_MISC, ITEM_SUBCLASS_JUNK, UNSET, MATERIAL_JEWELRY, COIN_DISPLAY, 0, 0])
    for coins, cost in sorted(costs.items()):
        row = [0] * COST_FIELDS
        row[0], row[COST_ITEM], row[COST_COUNT] = cost, COIN, coins
        prices.append(row)

    write_dbc(files[dbc("Item")], items, item_str, item_fields)
    write_dbc(files[dbc("ItemExtendedCost")], prices, price_str, price_fields)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sql", help="write the module's world SQL here")
    ap.add_argument("--patch-w", help="your realm's live spell and item patch (e.g. patch-W.MPQ)")
    ap.add_argument("--client", help="the base client folder (has Data/<locale>/); only needed the first time")
    ap.add_argument("--locale", default="enUS")
    ap.add_argument("--out", help="the new spell and item patch")
    args = ap.parse_args()
    if not args.sql and not args.patch_w:
        ap.error("nothing to do: give --sql, or --patch-w and --out")
    if bool(args.patch_w) != bool(args.out):
        ap.error("--patch-w and --out go together")

    costs, pages, sources, credits, shop = load()
    if args.sql:
        write_sql(args.sql, costs, pages, sources, credits, shop)
    if args.patch_w:
        with tempfile.TemporaryDirectory() as tmp:
            unpacked = os.path.join(tmp, "w")
            os.mkdir(unpacked)
            files = unpack(args.patch_w, unpacked)
            patch_w(files, costs, args, tmp)
            pack(args.out, [(local, name) for name, local in sorted(files.items())])
            print("%s: %d files, %.1f MB" % (args.out, len(files), os.path.getsize(args.out) / 1e6))


if __name__ == "__main__":
    main()
