# Boss Coins

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that adds the **Tarnished
Undermine Real**: a coin that level 60 dungeon and raid bosses give to every player in the
instance when they die, and a goblin in Booty Bay who trades set pieces, crafting materials and flask recipes
for it. It follows the coin of the same name in World of Warcraft: Season of Discovery.

The point is bad luck protection. A boss that doesn't drop your piece still moves you towards
buying it.

## Earning coins

| | Coins | |
| --- | --- | --- |
| Molten Core | 1 per boss | 10 for a full clear |
| Blackwing Lair | 1 per boss | 8 for a full clear |
| Onyxia's Lair (40 player) | 3 | the raid that mod-individual-progression adds |
| Zul'Gurub | 1 per boss | 10 for a full clear |
| Ruins of Ahn'Qiraj | 1 per boss | 6 for a full clear |
| Temple of Ahn'Qiraj | 1 per boss | 9 for a full clear |
| Naxxramas (40 player) | 1 per boss | 15 for a full clear; the raid that mod-individual-progression adds. The level 80 Naxxramas gives none |
| Blackrock Depths | 1 per boss of level 55 or higher | 11 with mod-individual-progression's boss levels, 9 on stock data |
| Blackrock Spire | 1 per boss | 14, Lower and Upper together |
| Scholomance | 1 per boss | 13 |
| Stratholme | 1 per boss | 13 |
| Dire Maul | 1 per boss | 16, all three wings |

- **Everyone in the instance gets their own coins** when the boss dies, straight into their
  bags. The dead get theirs too, as long as they are still inside. There is nothing to loot and
  nothing to roll for.
- **Once per boss.** A raid boss pays each character once per lockout: kill it again after the
  raid has reset and it pays again, whatever the hour. A dungeon boss pays once until the next
  daily quest reset, so resetting a dungeon and running it again gives nothing the second time.
- **Bots get none.** Only players with a game client count.
- A character can have **250** coins, counting the bank and the mailbox. Coins over that are
  lost. Coins are bind on pickup.
- With full bags the coins come by mail.

## Spending coins

**Pix Xizzix** stands on the Booty Bay boardwalk. She has nine pages:

| Page | Sells | Price in coins | Open to |
| --- | --- | --- | --- |
| Dungeon sets | Dungeon Set 1 (Valor, Lightforge, Magister's and the rest) | bracers 15; belt, gloves, boots, shoulders 25; helm, chest, legs 50 | everyone |
| Molten Core | Tier 1, and the Tier 2 legs (Ragnaros drops those) | bracers, belt, gloves 15; boots, shoulders 20; helm, chest, legs 25; Tier 2 legs 50 | players who have killed Ragnaros |
| Onyxia's Lair | Tier 2 helm | 50 | players who have killed Onyxia |
| Blackwing Lair | Tier 2 bracers, belt, gloves and boots | 35 each | players who have killed Nefarian |
| Zul'Gurub | the class sets, all five pieces | bracers 15; belt, shoulders 20; chest, necklace, trinket 25 | players who have killed Hakkar |
| Ruins of Ahn'Qiraj | the class sets, all three pieces | cloak, ring 25; weapon 35 | players who have killed Ossirian |
| Temple of Ahn'Qiraj | Tier 2.5, all five pieces | boots, shoulders 50; helm, chest, legs 65 | players who have killed C'Thun |
| Naxxramas | Tier 3, all nine pieces | bracers, belt, gloves, boots 50; shoulders 65; helm, chest, legs, ring 80 | players who have killed Kel'Thuzad |
| Supplies | Core Leather 10; Lava Core and Fiery Core 15; the four flask recipes and the Potion of Petrification recipe 50 | | players who have killed Ragnaros |

- She shows each class its own pieces only.
- Tier 2 shoulders and chest are not for sale. They still have to drop.
- "Has killed Ragnaros", "has killed Onyxia", "has killed Nefarian", "has killed C'Thun" and
  "has killed Kel'Thuzad" are
  [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  phases 1, 2, 3, 6 and 7. "Has killed Hakkar" and "has killed Ossirian" are the achievements
  Zul'Gurub and Ruins of Ahn'Qiraj, because those two raids are no phase of their own. See
  [Without mod-individual-progression](#without-mod-individual-progression).
- A player who has only the first page open still gets her greeting first, which says how the
  others open.

## Requirements

- AzerothCore 3.3.5a.
- **A client patch.** The coin is a new item and the prices are new rows of a table the client
  only reads from its own files. Without the patch the coin shows a question mark, Pix Xizzix's
  window shows no prices, and nothing can be bought.

## Install

```bash
cd azerothcore-wotlk/modules
git clone https://github.com/buildthehomelab/wow-mod-boss-coins.git mod-boss-coins
```

Clone into `mod-boss-coins`, without the `wow-`: AzerothCore names the module's loader after the
folder. Re-run CMake, build, and copy `conf/mod_boss_coins.conf.dist` to your config folder as
`mod_boss_coins.conf`. The world and characters database changes apply themselves at the next
start.

### The client patch

`tools/build_boss_coins.py` adds the coin and the prices to the patch MPQ that carries your
realm's `Item.dbc`:

```bash
python3 tools/build_boss_coins.py --patch-w patch-z80-spells-and-items.MPQ \
    --client /path/to/client --out patch-z80-spells-and-items-new.MPQ
```

- `--patch-w` is that patch. A patch MPQ replaces whole files, so the coin goes into its copy of
  `Item.dbc`, which has to be in it along with `ItemDisplayInfo.dbc`. Everything else in the
  patch is copied unchanged.
- `--client` is a 3.3.5a client folder. The first time, the patch has no `ItemExtendedCost.dbc`
  yet and the tool starts from the client's own. After that the option isn't needed.

Give players the new patch in place of the old one. Needs Python 3 and
[StormLib](https://github.com/ladislav-zezula/StormLib).

## Configuration

`mod_boss_coins.conf`:

| Setting | Default | |
| --- | --- | --- |
| `BossCoins.Enable` | `1` | With `0` no boss gives coins. Pix Xizzix keeps trading |
| `BossCoins.MinBossLevel` | `55` | A boss below this level gives nothing, unless it has a row of its own in the table below |

Both reload with `.reload config`.

### Which bosses give coins

World table `mod_boss_coins_source`, read at startup:

| Column | |
| --- | --- |
| `map` | the instance's map id |
| `encounter` | `0` for every boss of the map, or one boss's `DungeonEncounter.dbc` id. A boss's own row wins over its map's row |
| `coins` | how many. `0` switches the row off: on a boss's own row, that one boss |

A boss is whatever the core counts as one of the instance's encounters (`instance_encounters`).
To make Ragnaros pay three coins while the rest of Molten Core stays at one:

```sql
INSERT INTO mod_boss_coins_source (map, encounter, coins, comment) VALUES (409, 672, 3, 'Ragnaros');
```

Some bosses the core's own credit doesn't fit. World table `mod_boss_coins_credit`
(`creature`, `encounter`) names the creatures of such a boss. A boss with rows there is paid
by them alone: when the last of its creatures in the instance dies, killed by players. So list
every creature of the boss. Only creatures spawned from the `creature` table count as still
alive; ones a script summons aren't seen. The module fills it for three:

- Zul'Gurub's Edge of Madness, where Gri'lek, Hazza'rah, Renataki and Wushoolay take turns and
  the core only credits Hazza'rah.
- The bug trio of the Temple of Ahn'Qiraj, which the core credits as soon as Vem dies.
- The Four Horsemen of the 40 player Naxxramas, so that they pay when the fourth dies whether
  or not mod-individual-progression's credit spell goes off.

Restart the server after changing either table. The module only adds its own rows when they are
missing, so your changes survive updates.

### What Pix Xizzix sells

Her stock, pages and prices are the files in `data/`. Change them, then write the SQL again:

```bash
python3 tools/build_boss_coins.py --sql data/sql/db-world/mod_boss_coins_world.sql
```

- `stock.tsv`: a row per item: page, item id, price, and the class that sees it (empty for all).
- `pages.tsv`: her pages, and the progression phase or the achievement each one waits for.
- `costs.tsv`: one row per price. **A new price needs a new row here and a rebuilt client
  patch**: the client can only charge prices it has in its own table.

One page holds at most 150 items; the tool stops if a page goes over.

### Without mod-individual-progression

Six of the locked pages wait for that module's hidden quests (66001, 66002, 66003, 66006 and
66007). On a server
without it those quests don't exist, the core drops the conditions with an error in its log at
startup, and those pages are open to everyone. The Zul'Gurub and Ruins of Ahn'Qiraj pages wait for
achievements and stay locked as before. To get rid of the errors, set `phase` to `0` on
every row of `data/pages.tsv` and write the SQL again.

## What it adds to the databases

- World: item 90410, Tarnished Undermine Real. `itemextendedcost_dbc` rows 90411 to 90418.
  Creature 9503200, Pix Xizzix, with her spawn (guid 9503200), gossip menu and `npc_text`
  9503200, and creatures 9503201 to 9503209, which are never spawned: each owns one page of
  her stock. `conditions` rows on her gossip options and her stock. Tables
  `mod_boss_coins_source` and `mod_boss_coins_credit`.
- Characters: table `mod_boss_coins_award`, the lockout or day in which each character last
  got each boss's coins.
- This module's id ranges: item 90410, `ItemExtendedCost` 90411 to 90429, and 9503200 to
  9503299 for everything else. No stock rows are changed.

## Uninstalling

Remove the module folder and rebuild, then run the two files in `data/sql/uninstall/` by hand,
each on its own database. The characters file deletes every coin players own.

## Troubleshooting

- **The coin is a question mark, or Pix Xizzix's window shows no prices:** the client doesn't
  have the patch, or has an older copy of it.
- **A boss gave nothing:** a raid boss pays a character once per lockout, a dungeon boss once
  a day. Otherwise check that the instance has a row in `mod_boss_coins_source`, that the boss
  is level 55 or higher, and that the worldserver log has the `mod-boss-coins:` lines at
  startup.
- **Pix Xizzix only offers the dungeon sets:** the other pages wait for Ragnaros, Onyxia,
  Nefarian, Hakkar, Ossirian, C'Thun and Kel'Thuzad.
- **A page is empty:** she shows a class its own pieces. Game masters see the same as players.
- **The module doesn't build or load:** the folder must be named `mod-boss-coins`.

## Patch Notes: Tarnished Undermine Reals

Category: Dungeons & Raids

- Bosses in **Molten Core**, **Blackwing Lair**, the 40 player **Onyxia's Lair**, **Zul'Gurub**,
  both **Ahn'Qiraj** raids and the 40 player **Naxxramas** now give **Tarnished Undermine
  Reals** to every player in the raid. Bosses of level 55 and higher in
  **Blackrock Depths**, **Blackrock Spire**, **Scholomance**, **Stratholme** and **Dire Maul**
  give them too. A raid boss gives a character its coins once per lockout, a dungeon boss once
  per day.
- **Pix Xizzix** has set up shop in Booty Bay. She trades Dungeon Set 1 pieces for Reals, and
  Tier 1 pieces, Tier 2 legs, **Core Leather**, **Lava Cores**, **Fiery Cores** and flask
  recipes to players who have defeated Ragnaros. Players who have defeated Onyxia can buy their
  Tier 2 helm.
- Players who have defeated Nefarian can also buy Tier 2 bracers, belts, gloves and boots from
  her. Players who have defeated Hakkar or Ossirian can buy their class's **Zul'Gurub** or
  **Ruins of Ahn'Qiraj** set. Players who have defeated C'Thun can buy every Tier 2.5 piece, and players who have
  defeated Kel'Thuzad every Tier 3 piece, the ring included.

> Nobody should clear Molten Core sixteen times and still be missing their gloves. Every boss
> you kill now gets you a little closer to the piece that won't drop.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

Modelled on the Tarnished Undermine Real of World of Warcraft: Season of Discovery.

## License

MIT, see [LICENSE](LICENSE).
