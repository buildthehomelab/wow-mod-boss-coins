# Boss Coins

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that adds the **Tarnished
Undermine Real**: a coin that level 60 dungeon and raid bosses give to every player who helps
kill them, and a goblin in Booty Bay who trades set pieces, crafting materials and flask recipes
for it. It follows the coin of the same name in World of Warcraft: Season of Discovery.

The point is bad luck protection. A boss that doesn't drop your piece still moves you towards
buying it.

## Earning coins

| | Coins | |
| --- | --- | --- |
| Molten Core | 1 per boss | 10 for a full clear |
| Blackwing Lair | 1 per boss | 8 for a full clear |
| Onyxia's Lair (40 player) | 3 | the raid that mod-individual-progression adds |
| Blackrock Depths | 1 per boss of level 55 or higher | 11 |
| Blackrock Spire | 1 per boss | 14, Lower and Upper together |
| Scholomance | 1 per boss | 13 |
| Stratholme | 1 per boss | 13 |
| Dire Maul | 1 per boss | 16, all three wings |

- **Everyone in the instance gets their own coins** when the boss dies, straight into their
  bags. The dead get theirs too, as long as they are still inside. There is nothing to loot and
  nothing to roll for.
- **Once a day per boss.** A boss pays each character once until the next daily quest reset.
  Resetting a dungeon and running it again gives nothing the second time.
- **Bots get none.** Only players with a game client count.
- A character can own **250** coins, bank included. Coins are bind on pickup.
- With full bags the coins come by mail.

## Spending coins

**Pix Xizzix** stands on the Booty Bay boardwalk. She has four pages:

| Page | Sells | Price in coins | Open to |
| --- | --- | --- | --- |
| Dungeon sets | Dungeon Set 1 (Valor, Lightforge, Magister's and the rest) | bracers 15; belt, gloves, boots, shoulders 25; helm, chest, legs 50 | everyone |
| Molten Core | Tier 1 | bracers, belt, gloves 15; boots, shoulders 20; helm, chest, legs 25 | players who have killed Ragnaros |
| Blackwing Lair | Tier 2 bracers, belt, gloves and boots | 35 each | players who have killed Nefarian |
| Supplies | Core Leather 10; Lava Core and Fiery Core 15; the five flask recipes 50 | | players who have killed Ragnaros |

- She shows each class its own pieces only.
- Tier 2 helm, shoulders, chest and legs are not for sale. They still have to drop.
- "Has killed Ragnaros" and "has killed Nefarian" are
  [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  phases 1 and 3. See [Without mod-individual-progression](#without-mod-individual-progression).

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
| `BossCoins.MinBossLevel` | `55` | A boss below this level gives nothing |

Both reload with `.reload config`.

### Which bosses give coins

World table `mod_boss_coins_source`, read at startup:

| Column | |
| --- | --- |
| `map` | the instance's map id |
| `encounter` | `0` for every boss of the map, or one boss's `DungeonEncounter.dbc` id. A boss's own row wins over its map's row |
| `coins` | how many. `0` switches the row off |

A boss is whatever the core counts as one of the instance's encounters (`instance_encounters`).
To add Zul'Gurub at one coin a boss:

```sql
INSERT INTO mod_boss_coins_source (map, encounter, coins, comment) VALUES (309, 0, 1, 'Zul''Gurub');
```

Restart the server afterwards. The module only adds its own rows when they are missing, so your
changes survive updates.

### What Pix Xizzix sells

Her stock, pages and prices are the files in `data/`. Change them, then write the SQL again:

```bash
python3 tools/build_boss_coins.py --sql data/sql/db-world/mod_boss_coins_world.sql
```

- `stock.tsv`: a row per item: page, item id, price, and the class that sees it (empty for all).
- `pages.tsv`: her pages and the progression phase each one waits for.
- `costs.tsv`: one row per price. **A new price needs a new row here and a rebuilt client
  patch**: the client can only charge prices it has in its own table.

One page holds at most 150 items.

### Without mod-individual-progression

The three locked pages wait for that module's hidden quests (66001 and 66003). On a server
without it those quests don't exist, the core drops the conditions with an error in its log at
startup, and every page is open to everyone. To get rid of the errors, set `phase` to `0` on
every row of `data/pages.tsv` and write the SQL again.

## What it adds to the databases

- World: item 90410, Tarnished Undermine Real. `itemextendedcost_dbc` rows 90411 to 90416.
  Creature 9503200, Pix Xizzix, with her spawn (guid 9503200), gossip menu and `npc_text`
  9503200, and creatures 9503201 to 9503204, which are never spawned: each owns one page of
  her stock. `conditions` rows on her gossip options and her stock. Table
  `mod_boss_coins_source`.
- Characters: table `mod_boss_coins_award`, when each character last got each boss's coins.
- This module's id ranges: item 90410, `ItemExtendedCost` 90411 to 90429, and 9503200 to
  9503299 for everything else. No stock rows are changed.

## Uninstalling

Remove the module folder and rebuild, then run the two files in `data/sql/uninstall/` by hand,
each on its own database. The characters file deletes every coin players own.

## Troubleshooting

- **The coin is a question mark, or Pix Xizzix's window shows no prices:** the client doesn't
  have the patch, or has an older copy of it.
- **A boss gave nothing:** each boss pays a character once a day. Otherwise check that the
  instance has a row in `mod_boss_coins_source`, that the boss is level 55 or higher, and that
  the worldserver log has the `mod-boss-coins:` lines at startup.
- **Pix Xizzix only offers the dungeon sets:** the other pages wait for Ragnaros and Nefarian.
- **A page is empty:** she shows a class its own pieces. Game masters see the same as players.
- **The server log says a vendor has too many items:** a page went over 150. Split it in
  `data/pages.tsv`.
- **The module doesn't build or load:** the folder must be named `mod-boss-coins`.

## Patch Notes: Tarnished Undermine Reals

Category: Dungeons & Raids

- Bosses in **Molten Core**, **Blackwing Lair** and the 40 player **Onyxia's Lair** now give
  **Tarnished Undermine Reals** to every player in the raid. Bosses of level 55 and higher in
  **Blackrock Depths**, **Blackrock Spire**, **Scholomance**, **Stratholme** and **Dire Maul**
  give them too. Each boss gives a character its coins once per day.
- **Pix Xizzix** has set up shop in Booty Bay. She trades Dungeon Set 1 pieces for Reals, and
  Tier 1 pieces, **Core Leather**, **Lava Cores**, **Fiery Cores** and flask recipes to players
  who have defeated Ragnaros.
- Players who have defeated Nefarian can also buy Tier 2 bracers, belts, gloves and boots from
  her.

> Nobody should clear Molten Core sixteen times and still be missing their gloves. Every boss
> you kill now gets you a little closer to the piece that won't drop.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

Modelled on the Tarnished Undermine Real of World of Warcraft: Season of Discovery.

## License

MIT, see [LICENSE](LICENSE).
