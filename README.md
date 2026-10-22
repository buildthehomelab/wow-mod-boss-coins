# Boss Coins

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that adds the **Tarnished
Undermine Real**: a coin that level 60 dungeon and raid bosses give to every player in the
instance when they die, and a goblin in Booty Bay who trades set pieces, crafting materials and flask recipes
for it. It follows the coin of the same name in World of Warcraft: Season of Discovery.

The point is bad luck protection. A boss that doesn't drop your piece still moves you towards
buying it. And a couple of coins a day just for logging in, so everyone saves up a little.

Each account also gets a **coin wallet**, and there is a **shop** for mounts, companions and
character services (rename, customize, race and faction change) that a launcher or a website
can show: it buys through console commands over SOAP. See [The wallet and the shop](#the-wallet-and-the-shop).

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
  **account wallet** (see [The wallet and the shop](#the-wallet-and-the-shop)). The dead get
  theirs too, as long as they are still inside. There is nothing to loot and nothing to roll
  for.
- **Once per boss.** A raid boss pays each character once per lockout: kill it again after the
  raid has reset and it pays again, whatever the hour. A dungeon boss pays once until the next
  daily quest reset, so resetting a dungeon and running it again gives nothing the second time.
  A game master who takes a character off a raid lockout (`.instance unbind`) also lets them
  earn that raid's coins again, in the new instance. So does anything else that takes a
  character off a lockout: keep mod-playerbots' `Playerbots.ResetInstanceIdForAltBots` at 0,
  or players can do it for their own alts.
- **Bots get none.** Only players with a game client count.
- With `BossCoins.BossKills.ToWallet = 0` the coins go into the bags instead, and with full
  bags they come by mail.
- **Coins are bound to the account.** They can't be traded or sold, but a player can mail them
  to their other characters, of either faction, and the mail arrives at once. There is no cap
  on how many a character can own.

### Daily login coins

- **2 coins a day per account** for logging in, after the daily quest reset (06:00 server
  time), **into the account wallet**. The first character of the account in the world gets
  them for the account; the account's other characters get nothing more that day. Per account,
  not per character, because the coins can be mailed between the account's characters anyway.
- A player who is already in the world at the reset gets the new day's coins then.
- With `BossCoins.DailyLogin.ToWallet = 0` they go into the bags instead, by mail when the
  bags are full.
- Bots get none. `BossCoins.DailyLogin.Coins` sets the number; `0` switches it off.

## Spending coins

**Pix Xizzix** stands on the Booty Bay boardwalk. She has nine pages:

| Page | Sells | Price in coins | Open to |
| --- | --- | --- | --- |
| Dungeon sets | Dungeon Set 1 (Valor, Lightforge, Magister's and the rest) | bracers 15; belt, gloves, boots, shoulders 25; helm, chest, legs 50 | everyone |
| Molten Core | Tier 1, and the Tier 2 legs (Ragnaros drops those) | bracers, belt, gloves 15; boots, shoulders 20; helm, chest, legs 25; Tier 2 legs 50 | players who have killed Ragnaros |
| Onyxia's Lair | Tier 2 helm | 50 | players who have killed Onyxia |
| Blackwing Lair | Tier 2 bracers, belt, gloves, boots, shoulders and chest | bracers, belt, gloves, boots, shoulders 35; chest 50 | players who have killed Nefarian |
| Zul'Gurub | the class sets, all five pieces | bracers 15; belt, shoulders 20; chest, necklace, trinket 25 | players who have killed Hakkar |
| Ruins of Ahn'Qiraj | the class sets, all three pieces | cloak, ring 25; weapon 35 | players who have killed Ossirian |
| Temple of Ahn'Qiraj | Tier 2.5, all five pieces | boots, shoulders 50; helm, chest, legs 65 | players who have killed C'Thun |
| Naxxramas | Tier 3, all nine pieces | bracers, belt, gloves, boots 50; shoulders 65; helm, chest, legs, ring 80 | players who have killed Kel'Thuzad |
| Supplies | Core Leather 10; Lava Core and Fiery Core 15; the four flask recipes and the Potion of Petrification recipe 50 | | players who have killed Ragnaros |

- She shows each class its own pieces only.
- Every Tier 2 piece is for sale, on the page of the raid it drops in: legs from the Molten
  Core, helm from Onyxia's Lair, the other six from Blackwing Lair.
- "Has killed" is the raid's own achievement: Molten Core (Ragnaros), Onyxia's Lair (Level 60),
  Blackwing Lair (Nefarian), Zul'Gurub (Hakkar), Ruins of Ahn'Qiraj (Ossirian) and Temple of
  Ahn'Qiraj (C'Thun). So a page opens for the raid it belongs to only: killing Nefarian doesn't
  open Onyxia's Lair's page. "Has killed Kel'Thuzad" is achievement 533, which
  [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  adds and gives for killing its 40 player Kel'Thuzad. See
  [Without mod-individual-progression](#without-mod-individual-progression).
- A player who has only the first page open still gets her greeting first, which says how the
  others open.

## The wallet and the shop

Every account has a coin wallet. It has no cap, and neither do the coins in a character's bags.

- **In:** the boss coins, the daily login coins, and Pix Xizzix's option *Put the coins I
  carry in my account wallet*, which takes every coin the character has in bags and bank.
- **Out:** her option *Take coins out of my account wallet* offers 10, 25, 50, 100 or as many
  as the bags have room for (a 3.3.5 client can only ask for a typed number in
  an "Enter Code" box, so there is none). And the shop.
- **Buying from Pix:** her prices are bag coins, but the wallet pays too. When the bags are
  short of a price, she moves the missing coins out of the wallet into the bags first, all of
  them or none, and says so in chat (or why she can't: no room in the bags). If the purchase still fails after the
  move (the coins took the slot the item needed), the moved coins stay in the bags.
- `.wallet` shows the account's balance. `.wallet adjust <account> <coins>` (administrators,
  console too) adds coins, or with a minus takes them.
- **DragonUI's Currency tab** shows the balance too, under *Account Wallet* (with
  [DragonUI](https://github.com/buildthehomelab/wow-addon-DragonUI) 2.6.1 or later). The
  server sends it as an addon message (prefix `DUI_CURRENCY`) when the client asks at login or
  after a `/reload`, and again whenever the balance changes.
- Every change is in table `mod_boss_coins_ledger`: when, how many, from or to which character,
  and why.

The shop sells what world table `mod_boss_coins_shop` lists, for wallet coins: an item, which
is mailed to the character it is bought for (the mail keeps 180 days), or a service the character gets at the character
list. As shipped:

| Offer | Price |
| --- | --- |
| Character Rename | 20 |
| Character Customization (name, gender, looks) | 20 |
| Race Change | 50 |
| Faction Change (the mailbox must be empty) | 75 |
| The ten red panda companions of [mod-retail-pets](https://github.com/buildthehomelab/wow-mod-retail-pets) | 10 each |
| Its five Meeksi red panda mounts | 40 each |
| Its Whoofle Bramblewing, an owl mount that flies where flying mounts can | free |
| Its 69 retail shop mounts (Grrlocs, Chaos-Forged mounts, Witchwick's Riders, ...) | 40 + 5 per progression phase: 45 to 130 |

**Progression.** An offer can wait for a
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) phase
(the `phase` column). Until a character of the account has reached it, the offer is listed as
locked, with what unlocks it ("Defeat Ragnaros in Molten Core with any character to unlock it."), and the shop won't sell it to
any character of the account. The account's furthest character counts, so a main opens the shop
for its alts. The five Meeksi wait for the same phases as at Nari Rustpaw (1, 3, 6, 7 and 8). The 69 retail
mounts are spread out in their families, a few with every raid tier: the ground and water
mounts and two early flyers (the Wondrous Wavewhisker and the Kickin' Kezan Waveshredder) over the
vanilla phases (1-7), the other flyers over The Burning Crusade and Wrath of the Lich King (8-18),
when flying comes. An offer past mod-individual-progression's `ProgressionLimit` stays
locked and says which expansion brings it ("Comes to the realm with The Burning Crusade." for
phases 8-12, Wrath of the Lich King from 13, the Kil'jaeden kill that opens Northrend) until the
limit is raised. Without mod-individual-progression (no quest 66001 on the server) every
offer is for sale.

An item that isn't on the server is left out, so the pandas only show with that module
installed. A character that already has an item (in bags, bank or mail, or the spell it
teaches), or has the service waiting, isn't sold it again. Neither is a character that can't
use the item: its race or class, its level, or a skill it needs (a mount needs riding). A
price of 0 makes an offer free, with the same rules: one per character, and no coins move.

There is no shop window in the game. A launcher or website shows it, through two console
commands that it runs over [SOAP](https://www.azerothcore.org/wiki/remote-access) (with an
administrator account, as SOAP requires). Players can't run them, game masters neither. Both
answer one line of JSON:

- `.coinshop list <account id>`: the wallet balance, the account's characters, and every
  offer with the characters that own it already (`owned`) or can't use it (`cannot`), the
  account's progression `phase`, and per offer its `phase`, whether it is `locked` and what
  unlocks it (`unlock`). The command reads the account's characters from the database on the world thread: cache its
  answer for a little while rather than calling it on every click.
- `.coinshop buy <account id> <offer id> <character guid>`: `{"ok":true,"balance":..,"message":..}`,
  or `{"ok":false,"error":..,"message":..}` with error `closed`, `no_offer`, `no_character`,
  `busy` (a character of the account is logging in), `locked`, `owned`, `cannot` or `balance`. The coins and the order are written in one transaction.

The caller decides who the account is (a login of its own), so keep SOAP away from players.
`.coinshop reload` reads the table again.

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
realm's `Item.dbc` (`patch-W.MPQ` below is an example name):

```bash
python3 tools/build_boss_coins.py --patch-w patch-W.MPQ \
    --client /path/to/client --out patch-W-new.MPQ
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
| `BossCoins.Enable` | `1` | With `0` no boss gives coins and there are no daily login coins. Pix Xizzix keeps trading |
| `BossCoins.MinBossLevel` | `55` | A boss below this level gives nothing, unless it has a row of its own in the table below |
| `BossCoins.BossKills.ToWallet` | `1` | `1`: a boss's coins go into the account wallet. `0`: into the bags |
| `BossCoins.BossKills.BonusAura` | `0` | a spell whose aura gives a chance (its first effect amount, in percent) of one more coin per boss award, e.g. mod-guild-levels' For Great Justice (90455). `0`: off |
| `BossCoins.DailyLogin.Coins` | `2` | Coins an account gets once a day for logging in. `0` switches it off |
| `BossCoins.DailyLogin.ToWallet` | `1` | `1`: the daily coins go into the account wallet. `0`: into the bags |
| `BossCoins.Shop.Enable` | `1` | With `0` the `.coinshop` commands answer that the shop is closed. The wallet stays |
| `BossCoins.Shop.AccountWideSpells` | `0` | `1` if your server shares mounts and companions between an account's characters: a mount one character knows isn't sold to the others |
| `BossCoins.Shop.ProgressionGate` | `1` | `1`: an offer with a `phase` stays locked until a character of the account has reached that progression phase. `0`: everything is for sale to everyone |

All of them reload with `.reload config`.

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
- `pages.tsv`: her pages, and the achievement each one waits for.
- `costs.tsv`: one row per price. **A new price needs a new row here and a rebuilt client
  patch**: the client can only charge prices it has in its own table.

One page holds at most 150 items; the tool stops if a page goes over.

### Without mod-individual-progression

The Naxxramas page waits for that module's achievement 533. On a server without it the achievement
doesn't exist, the core drops the condition with an error in its log at startup, and that page is
open to everyone. The other locked pages wait for the client's own achievements and stay locked.
To lock the Naxxramas page anyway, set its `achievement` in `data/pages.tsv` to one your server
gives and write the SQL again.

## What it adds to the databases

- World: item 90410, Tarnished Undermine Real. `itemextendedcost_dbc` rows 90411 to 90418.
  Creature 9503200, Pix Xizzix, with her spawn (guid 9503200), gossip menu and `npc_text`
  9503200, and creatures 9503201 to 9503209, which are never spawned: each owns one page of
  her stock. `conditions` rows on her gossip options and her stock. Tables
  `mod_boss_coins_source`, `mod_boss_coins_credit` and `mod_boss_coins_shop` (offer ids below
  1000 are the module's: its SQL keeps them up to date but sets `price`, `sort` and `enabled`
  only on a new row, so prices you change stay; add your own offers from 1000).
- Characters: table `mod_boss_coins_award`, the lockout or day, and for a raid the instance,
  in which each character last got each boss's coins. Table `mod_boss_coins_daily`, the day
  each account last got its daily login coins. Tables `mod_boss_coins_wallet` (balances) and
  `mod_boss_coins_ledger` (every change).
- This module's id ranges: item 90410, `ItemExtendedCost` 90411 to 90429, and 9503200 to
  9503299 for everything else. No stock rows are changed.

## Uninstalling

Remove the module folder and rebuild, then run the two files in `data/sql/uninstall/` by hand,
each on its own database. The characters file deletes every coin players own, wallets included.

## Troubleshooting

- **The coin is a question mark, or Pix Xizzix's window shows no prices:** the client doesn't
  have the patch, or has an older copy of it.
- **A boss gave nothing:** a raid boss pays a character once per lockout, a dungeon boss once
  a day. A boss that a game master respawns in the same raid instance doesn't pay again; a new
  instance after `.instance unbind` does. The coins are in the account wallet, not the bags:
  look in `.wallet`. Otherwise check that the instance has a row in `mod_boss_coins_source`, that the boss
  is level 55 or higher, and that the worldserver log has the `mod-boss-coins:` lines at
  startup.
- **No daily login coins:** the account already had them today, on another character (look in
  `.wallet`). The day starts at the daily quest reset, not at midnight.
- **Pix Xizzix has no wallet options:** the character carries no coins and the wallet is empty.
- **A shop item never arrived:** it is in the mail, from Pix Xizzix. `mod_boss_coins_ledger`
  has every purchase.
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
- Boss Reals now go straight into your **account wallet**, like the daily login Reals, so
  every character on your account saves towards the same pile and there is no 250 cap. Type
  `.wallet` to see it. Spend them in the launcher's shop, or take them out at Pix Xizzix to buy
  from her.
- **Pix Xizzix** has set up shop in Booty Bay. She trades Dungeon Set 1 pieces for Reals, and
  Tier 1 pieces, Tier 2 legs, **Core Leather**, **Lava Cores**, **Fiery Cores** and flask
  recipes to players who have defeated Ragnaros. Players who have defeated Onyxia can buy their
  Tier 2 helm.
- Players who have defeated Nefarian can also buy Tier 2 bracers, belts, gloves, boots,
  shoulders and chests from her. Players who have defeated Hakkar or Ossirian can buy their class's **Zul'Gurub** or
  **Ruins of Ahn'Qiraj** set. Players who have defeated C'Thun can buy every Tier 2.5 piece, and players who have
  defeated Kel'Thuzad every Tier 3 piece, the ring included.

- Tarnished Undermine Reals no longer stop at **250** a character: carry, bank and mail as
  many as you like.
- Every account now gets **2 Tarnished Undermine Reals a day** for logging in, after the
  daily reset. They go into your new **account wallet**: type `.wallet` to see it.
- Pix Xizzix keeps your wallet. She puts the Reals you carry into it, and pays them back out
  when you want them. Buying from her takes any Reals your bags are short of straight from your
  wallet, so there's no need to take them out first.
- Your wallet now shows in the **Currency** tab of the character window, under
  *Account Wallet*, so you can see your Reals without typing `.wallet`.
- The wallet pays for the new **coin shop**: character renames, new looks, race and faction
  changes, red panda companions and red panda mounts. Bought items arrive by mail.
- **69 new mounts** in the coin shop, from Grrlocs to Chaos-Forged gryphons and Witchwick's
  Riders. Most fly, wherever flying mounts can. They unlock as you progress: each shows what
  opens it, and your furthest character on the account opens it for all of them.
- The Great Panda mounts are now the **Meeksi**: Rollingpaw, Rufflefur, Softpaw, Teatuft and
  Brewthief. In the shop they unlock with the same raids as at Nari Rustpaw.

> Nobody should clear Molten Core sixteen times and still be missing their gloves. Every boss
> you kill now gets you a little closer to the piece that won't drop.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

Modelled on the Tarnished Undermine Real of World of Warcraft: Season of Discovery.

## License

MIT, see [LICENSE](LICENSE).
