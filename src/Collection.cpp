/*
 * mod-boss-coins: mounts and companions belong to the account.
 *
 * On a server that shares mounts and companions between an account's characters
 * (mod-accountwide-mounts and mod-accountwide-pets: their tables accountwide_mounts /
 * accountwide_pets, and their Enable settings on), the item that teaches one is only a way to the
 * spell.
 * So a soulbound one (bind on pickup, or a quest item) is collected for the ACCOUNT:
 *
 *   - The moment a character gets one the account doesn't have (loot, a quest reward, a vendor,
 *     a roll), the spell goes in the sharing module's table and the item is gone. The character
 *     learns it at once if it can use the item; the account's other characters learn it at their
 *     next login (the sharing module). A level 20 alt can loot a level 60 mount for its account.
 *   - Right-clicking one does the same, whatever the item asks of the character: one taken out
 *     of the mail, or one from before the module did this.
 *   - A mount or companion that an achievement gives is given once per ACCOUNT. Nothing is
 *     mailed: the first character to earn the achievement collects it, the next ones get nothing
 *     (titles still come). Which accounts had it is kept in mod_boss_coins_collection_achievement,
 *     which deleting characters doesn't touch, so a new alt can't earn it again for a spare. An
 *     account that lost the spell (the sharing modules forget an account's spells when its last
 *     character is deleted) gets the spell back, never an item.
 *   - A spare from a drop is worth coins: right-click one the account has already and a box
 *     offers to trade it for Tarnished Undermine Reals, paid into the account wallet (or, when
 *     this character doesn't know it yet and can use it, to learn it and keep the spare). Only
 *     for items that drop (a creature, a chest, fishing, skinning, a pocket, or a container that
 *     nothing else gives) and that no vendor sells, no quest, mail, spell or achievement gives
 *     and the launcher's shop doesn't sell: anything else would turn gold, a craft, or a quest
 *     done again on an alt, into coins. How many: by quality
 *     (BossCoins.Collection.Exchange.*), or world table mod_boss_coins_exchange per item.
 *
 * Bind on equip and bind on use items are left alone: they can still be sold. A bot's pickups and
 * achievements count for its account like a player's (an alt the player lets a bot run is still
 * theirs); only the right-click box is for players.
 *
 * Released under the MIT License.
 */

#include "AchievementMgr.h"
#include "BossCoins.h"
#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "DatabaseEnv.h"
#include "GossipDef.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace BossCoins;

namespace
{
    // The sharing modules' own on/off settings, by AccountKind.
    constexpr char const* KIND_ENABLE_OPTIONS[] = { "", "AccountWideMounts.Enable", "AccountWidePets.Enable" };

    // Item spells that teach the spell in the item's second spell slot. Mount and companion items
    // use one of these two.
    constexpr int32 SPELL_LEARNING = 483;
    constexpr int32 SPELL_LEARNING_COMPANION = 55884;

    // The trade-a-spare box: its gossip menu id, which is also its npc_text (the module's world
    // SQL), from Pix Xizzix's range.
    constexpr uint32 MENU_EXCHANGE = 9503210;
    // On the box's option sender, with the item entry: learn it on this character, don't trade.
    constexpr uint32 SENDER_LEARN = 0x80000000;

    // Right-clicks of these items closer together than this are dropped: each can cost two
    // queries.
    constexpr uint64 USE_INTERVAL_MS = 1000;

    // How long a spell given to an account counts as given without the database: its write is
    // asynchronous.
    constexpr uint64 RECENT_GRANT_MS = 60 * 1000;

    // Milliseconds on a clock that never wraps or jumps (the game's own ms time is 32 bits).
    uint64 NowMs()
    {
        return uint64(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    constexpr char const* QUEST_REWARD_COLUMNS[] = { "RewardItem1", "RewardItem2", "RewardItem3", "RewardItem4",
        "RewardChoiceItemID1", "RewardChoiceItemID2", "RewardChoiceItemID3", "RewardChoiceItemID4", "RewardChoiceItemID5",
        "RewardChoiceItemID6" };

    struct CollectionConfig
    {
        bool enabled = true;
        bool achievements = true;
        uint32 coinsCommon = 2;
        uint32 coinsRare = 5;
        uint32 coinsEpicCompanion = 15;
        uint32 coinsEpicMount = 25;
    };

    CollectionConfig collectionConfig;

    // Set at startup, read-only after.
    bool kindTables[3] = { false, false, false };
    // The sharing modules' Enable settings (config load, world thread).
    bool kindEnabled[3] = { false, false, false };
    std::unordered_set<uint32> dropItems;                  // items a spare of may be traded
    std::unordered_map<uint32, uint32> exchangeOverrides;  // mod_boss_coins_exchange: item -> coins

    // Achievements whose mount or companion the module gives instead of the mail: achievement ->
    // item. `originalRewards` keeps the mail as the core had it, to give it back when the setting
    // is turned off.
    std::unordered_map<uint32, uint32> achievementItems;
    struct OriginalReward
    {
        uint32 itemId = 0;
        uint32 sender = 0;
        uint8 kind = ACCOUNT_NONE;
    };
    std::unordered_map<uint32, OriginalReward> originalRewards;

    // (account << 32 | achievement) of every achievement mount or companion given. Achievements
    // complete on the map threads.
    std::mutex achievementsLock;
    std::unordered_set<uint64> achievementsGiven;

    // (account << 32 | spell) given in the last RECENT_GRANT_MS -> when: a second copy right
    // after the first is a spare, even before the first one's write is in the database.
    std::mutex recentGrantsLock;
    std::unordered_map<uint64, uint64> recentGrants;

    struct LastUse : DataMap::Base
    {
        uint64 ms = 0;
    };

    constexpr char const* LAST_USE_KEY = "BossCoinsCollectionUse";

    // Items a character got that wait for its next update to be collected: StoreNewItem's callers
    // still use the item after the hook, so it can't be destroyed there.
    struct PendingItems : DataMap::Base
    {
        std::vector<ObjectGuid> items;
    };

    constexpr char const* PENDING_KEY = "BossCoinsCollection";

    // Players with items waiting, so the per-tick update hook costs one load when there are none.
    std::atomic<uint32> playersPending{ 0 };

    uint64 Key(uint32 high, uint32 low)
    {
        return (uint64(high) << 32) | low;
    }

    // The spell an item teaches and what it is, when it teaches a mount or companion that the
    // server shares between characters.
    bool Teaches(ItemTemplate const* proto, uint32& spell, uint8& kind, bool evenIfOff = false)
    {
        if (!proto || (proto->Spells[0].SpellId != SPELL_LEARNING && proto->Spells[0].SpellId != SPELL_LEARNING_COMPANION))
            return false;

        if (proto->Spells[1].SpellId <= 0 || !sSpellMgr->GetSpellInfo(proto->Spells[1].SpellId))
            return false;

        spell = uint32(proto->Spells[1].SpellId);
        kind = AccountKindOf(spell);
        return kind != ACCOUNT_NONE && kindTables[kind] && (kindEnabled[kind] || evenIfOff);
    }

    // One the module collects for the account: soulbound, so it can't be sold or traded anyway.
    bool Collectable(ItemTemplate const* proto, uint32& spell, uint8& kind)
    {
        if (!collectionConfig.enabled || !proto)
            return false;

        if (proto->Bonding != BIND_WHEN_PICKED_UP && proto->Bonding != BIND_QUEST_ITEM)
            return false;

        return Teaches(proto, spell, kind);
    }

    // Coins for a spare, 0 when it can't be traded.
    uint32 ExchangeCoins(ItemTemplate const* proto, uint8 kind)
    {
        if (!sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL))
            return 0;

        // A row only prices what drops: on any other item it would turn gold or quests into coins.
        if (!dropItems.count(proto->ItemId))
            return 0;

        if (auto itr = exchangeOverrides.find(proto->ItemId); itr != exchangeOverrides.end())
            return itr->second;

        if (proto->Quality >= ITEM_QUALITY_EPIC)
            return kind == ACCOUNT_MOUNTS ? collectionConfig.coinsEpicMount : collectionConfig.coinsEpicCompanion;
        if (proto->Quality == ITEM_QUALITY_RARE)
            return collectionConfig.coinsRare;
        return collectionConfig.coinsCommon;
    }

    // Whether the account has the spell: this character knows it, it is in the sharing module's
    // table, or another character of the account knows it. Mounts and companions come rarely, so
    // the queries are run where they are needed.
    bool AccountHas(Player* player, uint32 spell, uint8 kind)
    {
        if (player->HasSpell(spell))
            return true;

        uint32 account = player->GetSession()->GetAccountId();
        {
            std::lock_guard<std::mutex> guard(recentGrantsLock);
            uint64 now = NowMs();
            for (auto itr = recentGrants.begin(); itr != recentGrants.end();)
                itr = now - itr->second > RECENT_GRANT_MS ? recentGrants.erase(itr) : std::next(itr);
            if (recentGrants.count(Key(account, spell)))
                return true;
        }

        if (CharacterDatabase.Query("SELECT 1 FROM `{}` WHERE `account_id` = {} AND `spell` = {} LIMIT 1",
            ACCOUNT_TABLES[kind], account, spell))
            return true;

        return CharacterDatabase.Query("SELECT 1 FROM character_spell s JOIN characters c ON c.guid = s.guid "
            "WHERE c.account = {} AND c.deleteDate IS NULL AND s.spell = {} LIMIT 1", account, spell) != nullptr;
    }

    std::string Bracketed(ItemTemplate const* proto)
    {
        return "[" + proto->Name1 + "]";
    }

    // Writes the spell to the account and teaches the character if it can use the item. `item`,
    // when there is one, is destroyed in the same transaction as the write.
    void GiveAccount(Player* player, ItemTemplate const* proto, uint32 spell, uint8 kind, Item* item)
    {
        uint32 account = player->GetSession()->GetAccountId();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append("INSERT IGNORE INTO `{}` (`account_id`, `spell`) VALUES ({}, {})", ACCOUNT_TABLES[kind], account, spell);
        if (item)
        {
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            player->SaveInventoryAndGoldToDB(trans);
        }
        CharacterDatabase.CommitTransaction(trans);
        {
            std::lock_guard<std::mutex> guard(recentGrantsLock);
            recentGrants[Key(account, spell)] = NowMs();
        }

        bool learned = !player->HasSpell(spell) && player->CanUseItem(proto) == EQUIP_ERR_OK;
        if (learned)
            player->learnSpell(spell);

        if (IsRealPlayer(player))
            ChatHandler(player->GetSession()).PSendSysMessage(learned
                ? "{} is in your account's collection now: you know it, and your other characters learn it when they log in."
                : "{} is in your account's collection now: your characters learn it when they log in.", Bracketed(proto));

        LOG_INFO("module", "mod-boss-coins: {} ({}) collected {} ({}) for account {}{}", player->GetName(),
            player->GetGUID().GetCounter(), proto->Name1, proto->ItemId, account, item ? "" : ", from an achievement");
    }

    void ShowExchange(Player* player, Item* item, uint8 kind, uint32 coins)
    {
        ItemTemplate const* proto = item->GetTemplate();
        ClearGossipMenuFor(player);
        player->PlayerTalkClass->GetGossipMenu().SetMenuId(MENU_EXCHANGE);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, fmt::format("Trade this spare {} for {} {}.", proto->Name1, coins, CoinName(coins)),
            proto->ItemId, item->GetGUID().GetCounter(),
            fmt::format("Trade your spare {} for {} {}? The coins go {}. Your account keeps the {} itself.",
                proto->Name1, coins, CoinName(coins), IsSelfFound(player->GetGUID().GetCounter()) ? "in your bags" : "to your account wallet",
                kind == ACCOUNT_MOUNTS ? "mount" : "companion"),
            0, false);
        uint32 spell = uint32(proto->Spells[1].SpellId);
        if (!player->HasSpell(spell) && player->CanUseItem(proto) == EQUIP_ERR_OK)
            AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Learn it on this character and keep the spare.",
                SENDER_LEARN | proto->ItemId, item->GetGUID().GetCounter());
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Keep it.", 0, 0);
        SendGossipMenuFor(player, MENU_EXCHANGE, player->GetGUID());
    }

    void Exchange(Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        if (!Collectable(proto, spell, kind))
            return;

        // The account lost it meanwhile (its last character was deleted): collected instead.
        if (!AccountHas(player, spell, kind))
        {
            GiveAccount(player, proto, spell, kind, item);
            return;
        }

        uint32 coins = ExchangeCoins(proto, kind);
        if (!coins)
            return;

        ChatHandler chat(player->GetSession());
        uint32 account = player->GetSession()->GetAccountId();
        uint32 guid = player->GetGUID().GetCounter();

        // A Self-Found character (mod-challenges) keeps the coins it earns, in its bags.
        if (IsSelfFound(guid))
        {
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            GiveCoinsToBags(player, coins);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            player->SaveInventoryAndGoldToDB(trans);
            CharacterDatabase.CommitTransaction(trans);

            chat.PSendSysMessage("You traded your spare {} for {} {}.", Bracketed(proto), coins, CoinName(coins));
            LOG_INFO("module", "mod-boss-coins: {} ({}, account {}, Self-Found) traded a spare {} ({}) for {} coins", player->GetName(),
                guid, account, proto->Name1, proto->ItemId, coins);
            return;
        }

        // Into the wallet and out of the bags in one transaction.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 balance = 0;
        if (!Wallet::Add(account, guid, coins, "exchange", 0, trans, &balance))
        {
            chat.SendSysMessage("Your account wallet is full.");
            return;
        }

        player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        chat.PSendSysMessage("You traded your spare {} for {} {}. Your account wallet now holds {}.",
            Bracketed(proto), coins, CoinName(coins), balance);
        LOG_INFO("module", "mod-boss-coins: {} ({}, account {}) traded a spare {} ({}) for {} coins", player->GetName(), guid,
            account, proto->Name1, proto->ItemId, coins);
    }

    // A new item, one update after it came: collected, or the player is told what a spare is
    // worth.
    void Collect(Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        if (!Collectable(proto, spell, kind))
            return;

        if (!AccountHas(player, spell, kind))
        {
            GiveAccount(player, proto, spell, kind, item);
            return;
        }

        if (!IsRealPlayer(player))
            return;

        ChatHandler chat(player->GetSession());
        if (uint32 coins = ExchangeCoins(proto, kind))
            chat.PSendSysMessage("Your account has {} already. Right-click it to trade it for {} {}.", Bracketed(proto), coins, CoinName(coins));
        else
            chat.PSendSysMessage("Your account has {} already.", Bracketed(proto));
    }

    // Items whose spares may be traded: the soulbound mount and companion items that drop, less
    // those that can be had another way.
    void LoadDropItems()
    {
        dropItems.clear();

        std::unordered_set<uint32> candidates;
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            // Settings can change at .reload config; the list can't: made as if everything were on.
            uint32 spell = 0;
            uint8 kind = ACCOUNT_NONE;
            if ((proto.Bonding == BIND_WHEN_PICKED_UP || proto.Bonding == BIND_QUEST_ITEM) && Teaches(&proto, spell, kind, true))
                candidates.insert(entry);
        }

        if (candidates.empty())
            return;

        std::string ids = IdList(candidates);
        auto collect = [&](std::string const& sql, std::unordered_set<uint32>& into)
        {
            if (QueryResult result = WorldDatabase.Query(sql))
            {
                do
                    into.insert(result->Fetch()[0].Get<uint32>());
                while (result->NextRow());
            }
        };

        // What can be had another way than a drop: what vendors sell, the shop sells, quests and
        // their mail give, spells make, and achievements give (the table, not the core's copy: the module takes
        // the items out of that, see StripAchievementRewards).
        auto obtainable = [&](std::unordered_set<uint32> const& items)
        {
            std::unordered_set<uint32> found;
            if (items.empty())
                return found;

            std::string list = IdList(items);
            collect(fmt::format("SELECT DISTINCT item FROM npc_vendor WHERE item IN ({})", list), found);
            collect(fmt::format("SELECT DISTINCT item FROM game_event_npc_vendor WHERE item IN ({})", list), found);
            collect(fmt::format("SELECT DISTINCT item FROM mod_boss_coins_shop WHERE item IN ({})", list), found);
            collect(fmt::format("SELECT DISTINCT Item FROM mail_loot_template WHERE Item IN ({})", list), found);
            collect(fmt::format("SELECT DISTINCT Item FROM spell_loot_template WHERE Item IN ({})", list), found);

            // Every reward column of a quest in one pass.
            std::string where;
            for (char const* column : QUEST_REWARD_COLUMNS)
                where += fmt::format("{}{} IN ({})", where.empty() ? "" : " OR ", column, list);
            std::string columns;
            for (char const* column : QUEST_REWARD_COLUMNS)
                columns += fmt::format("{}{}", columns.empty() ? "" : ", ", column);
            if (QueryResult result = WorldDatabase.Query("SELECT {} FROM quest_template WHERE {}", columns, where))
            {
                do
                {
                    Field* fields = result->Fetch();
                    for (size_t i = 0; i < std::size(QUEST_REWARD_COLUMNS); ++i)
                        if (uint32 item = fields[i].Get<uint32>(); items.count(item))
                            found.insert(item);
                } while (result->NextRow());
            }

            // Made by a spell: a craft, a loot card, a toy. Whoever can cast it can make spares.
            for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
                if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(id))
                    for (SpellEffectInfo const& effect : spellInfo->Effects)
                        if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM || effect.Effect == SPELL_EFFECT_CREATE_ITEM_2)
                            && items.count(effect.ItemType))
                            found.insert(effect.ItemType);
            collect(fmt::format("SELECT DISTINCT ItemID FROM achievement_reward WHERE ItemID IN ({})", list), found);
            return found;
        };

        // The loot a kill, a chest, fishing, skinning or a pocket gives, and every reference
        // table that loot reaches (references can nest).
        constexpr char const* SOURCES[] = { "creature_loot_template", "gameobject_loot_template", "fishing_loot_template",
            "skinning_loot_template", "pickpocketing_loot_template" };
        std::unordered_set<uint32> references;
        for (char const* table : SOURCES)
            collect(fmt::format("SELECT DISTINCT Reference FROM {} WHERE Reference > 0", table), references);
        for (size_t known = 0; !references.empty() && known != references.size();)
        {
            known = references.size();
            collect(fmt::format("SELECT DISTINCT Reference FROM reference_loot_template WHERE Reference > 0 AND Entry IN ({})",
                IdList(references)), references);
        }

        std::unordered_set<uint32> drops;
        for (char const* table : SOURCES)
            collect(fmt::format("SELECT DISTINCT Item FROM {} WHERE Reference = 0 AND Item IN ({})", table, ids), drops);
        if (!references.empty())
            collect(fmt::format("SELECT DISTINCT Item FROM reference_loot_template WHERE Reference = 0 AND Item IN ({}) AND Entry IN ({})",
                ids, IdList(references)), drops);

        // A container counts unless it can be had another way itself (a box bought for gold or
        // coins, a quest's satchel).
        std::unordered_set<uint32> containers;
        collect(fmt::format("SELECT DISTINCT Entry FROM item_loot_template WHERE Reference = 0 AND Item IN ({})", ids), containers);
        std::unordered_set<uint32> boughtContainers = obtainable(containers);
        for (uint32 container : boughtContainers)
            containers.erase(container);
        if (!containers.empty())
            collect(fmt::format("SELECT DISTINCT Item FROM item_loot_template WHERE Reference = 0 AND Item IN ({}) AND Entry IN ({})",
                ids, IdList(containers)), drops);

        std::unordered_set<uint32> other = obtainable(drops);
        for (uint32 item : drops)
            if (!other.count(item))
                dropItems.insert(item);
    }

    void LoadExchangeOverrides()
    {
        exchangeOverrides.clear();
        if (QueryResult result = WorldDatabase.Query("SELECT item, coins FROM mod_boss_coins_exchange"))
        {
            do
            {
                Field* fields = result->Fetch();
                exchangeOverrides[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
            } while (result->NextRow());
        }
    }

    // The core mails an achievement's item from its own copy of achievement_reward. For the
    // mounts and companions the module gives, that copy's item and mail are taken out (titles
    // are given apart from the mail and stay), and put back when the setting is off. A row that
    // has its item or mail again was reloaded (.reload achievement_reward): what it has now is
    // the original, and it is stripped again. Only on the world thread (startup, .reload config,
    // the world update), when no map is being updated.
    void StripAchievementRewards()
    {
        bool strip = collectionConfig.enabled && collectionConfig.achievements;
        for (uint32 id = 0; id < sAchievementStore.GetNumRows(); ++id)
        {
            AchievementEntry const* achievement = sAchievementStore.LookupEntry(id);
            if (!achievement)
                continue;

            AchievementReward* reward = const_cast<AchievementReward*>(sAchievementMgr->GetAchievementReward(achievement));
            if (!reward)
                continue;

            auto original = originalRewards.find(id);
            if (reward->itemId || reward->sender)
            {
                uint32 spell = 0;
                uint8 kind = ACCOUNT_NONE;
                if (!reward->itemId || !Teaches(sObjectMgr->GetItemTemplate(reward->itemId), spell, kind, true))
                {
                    // A reload changed it to something else.
                    if (original != originalRewards.end())
                    {
                        originalRewards.erase(original);
                        achievementItems.erase(id);
                    }
                    continue;
                }

                original = originalRewards.insert_or_assign(id, OriginalReward{ reward->itemId, reward->sender, kind }).first;
                achievementItems[id] = reward->itemId;
            }
            else if (original == originalRewards.end())
                continue;

            bool stripThis = strip && kindEnabled[original->second.kind];
            reward->itemId = stripThis ? 0 : original->second.itemId;
            reward->sender = stripThis ? 0 : original->second.sender;
        }
    }

    // Whether a reward the module strips has its item or mail back: `.reload achievement_reward`.
    bool RewardsReloaded()
    {
        if (!collectionConfig.enabled || !collectionConfig.achievements)
            return false;

        for (auto const& [id, original] : originalRewards)
        {
            if (!kindEnabled[original.kind])
                continue;

            AchievementEntry const* achievement = sAchievementStore.LookupEntry(id);
            AchievementReward const* reward = achievement ? sAchievementMgr->GetAchievementReward(achievement) : nullptr;
            if (reward && (reward->itemId || reward->sender))
                return true;
        }
        return false;
    }

    void LoadAchievementsGiven()
    {
        std::lock_guard<std::mutex> guard(achievementsLock);
        achievementsGiven.clear();
        if (QueryResult result = CharacterDatabase.Query("SELECT account, achievement FROM mod_boss_coins_collection_achievement"))
        {
            do
            {
                Field* fields = result->Fetch();
                achievementsGiven.insert(Key(fields[0].Get<uint32>(), fields[1].Get<uint32>()));
            } while (result->NextRow());
        }
    }
}

void BossCoins::Collection::Load()
{
    for (uint8 kind = ACCOUNT_MOUNTS; kind <= ACCOUNT_PETS; ++kind)
    {
        kindTables[kind] = FindAccountTable(kind);
    }

    if (!kindTables[ACCOUNT_MOUNTS] && !kindTables[ACCOUNT_PETS])
    {
        LOG_INFO("server.loading", "mod-boss-coins: no accountwide_mounts or accountwide_pets table; mounts and companions stay with "
            "the character that gets them");
        return;
    }

    LoadExchangeOverrides();
    LoadDropItems();
    LoadAchievementsGiven();
    StripAchievementRewards();

    LOG_INFO("server.loading", "mod-boss-coins: mounts and companions are collected for the account ({}{}); {} drop as tradeable "
        "spares, {} come from achievements", kindTables[ACCOUNT_MOUNTS] ? "mounts" : "",
        kindTables[ACCOUNT_MOUNTS] && kindTables[ACCOUNT_PETS] ? " and companions" : kindTables[ACCOUNT_PETS] ? "companions" : "",
        dropItems.size(), achievementItems.size());
}

class BossCoinsCollectionWorldScript : public WorldScript
{
public:
    BossCoinsCollectionWorldScript() : WorldScript("BossCoinsCollectionWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    // After the maps' update, on the world thread. A `.reload achievement_reward` runs before the
    // maps, so the items can be mailed for that one update; from the next one they're stripped.
    void OnUpdate(uint32 /*diff*/) override
    {
        if (RewardsReloaded())
        {
            StripAchievementRewards();
            LOG_INFO("module", "mod-boss-coins: achievement rewards were reloaded; mounts and companions are given to the account again, not mailed");
        }
    }

    void OnAfterConfigLoad(bool reload) override
    {
        collectionConfig.enabled = sConfigMgr->GetOption<bool>("BossCoins.Collection.Enable", true);
        collectionConfig.achievements = sConfigMgr->GetOption<bool>("BossCoins.Collection.Achievements", true);
        collectionConfig.coinsCommon = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.Common", 2);
        collectionConfig.coinsRare = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.Rare", 5);
        collectionConfig.coinsEpicCompanion = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.EpicCompanion", 15);
        collectionConfig.coinsEpicMount = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.EpicMount", 25);
        // The sharing modules' own settings, with their default. A table left behind by a removed
        // module can't be told from a live one: turn BossCoins.Collection.Enable off with it.
        for (uint8 kind = ACCOUNT_MOUNTS; kind <= ACCOUNT_PETS; ++kind)
            kindEnabled[kind] = sConfigMgr->GetOption<bool>(KIND_ENABLE_OPTIONS[kind], true, false);

        // At startup the achievement rewards aren't loaded yet: Collection::Load does it then.
        if (reload)
            StripAchievementRewards();
    }
};

class BossCoinsCollectionPlayerScript : public PlayerScript
{
public:
    BossCoinsCollectionPlayerScript() : PlayerScript("BossCoinsCollectionPlayerScript",
        { PLAYERHOOK_ON_STORE_NEW_ITEM, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_GOSSIP_SELECT,
          PLAYERHOOK_ON_ACHI_COMPLETE }) { }

    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/) override
    {
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        // Not in the world: a character being created, whose Player never logs out.
        if (!item || !player->IsInWorld() || !player->GetSession() || !Collectable(item->GetTemplate(), spell, kind))
            return;

        PendingItems* pending = player->CustomData.GetDefault<PendingItems>(PENDING_KEY);
        if (pending->items.empty())
            ++playersPending;
        pending->items.push_back(item->GetGUID());
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!playersPending.load(std::memory_order_relaxed))
            return;

        PendingItems* pending = player->CustomData.Get<PendingItems>(PENDING_KEY);
        if (!pending || pending->items.empty())
            return;

        std::vector<ObjectGuid> items;
        items.swap(pending->items);

        // One in the trade window waits until the trade is done or cancelled. One that moved to
        // the bank or is gone is left: right-clicking it collects it.
        for (ObjectGuid const& guid : items)
        {
            Item* item = player->GetItemByGuid(guid);
            if (!item || player->IsBankPos(item->GetPos()))
                continue;
            if (item->IsInTrade())
                pending->items.push_back(guid);
            else
                Collect(player, item);
        }

        if (pending->items.empty())
            --playersPending;
    }

    // What hadn't been collected yet stays an item: right-clicking it collects it.
    void OnPlayerLogout(Player* player) override
    {
        if (PendingItems* pending = player->CustomData.Get<PendingItems>(PENDING_KEY); pending && !pending->items.empty())
        {
            pending->items.clear();
            --playersPending;
        }
    }

    void OnPlayerGossipSelect(Player* player, uint32 menuId, uint32 sender, uint32 action) override
    {
        if (menuId != MENU_EXCHANGE)
            return;

        CloseGossipMenuFor(player);
        if (!action || !collectionConfig.enabled)
            return;

        Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(action));
        if (!item || item->GetEntry() != (sender & ~SENDER_LEARN) || player->IsBankPos(item->GetPos()) || item->IsInTrade())
            return;

        if (!(sender & SENDER_LEARN))
        {
            Exchange(player, item);
            return;
        }

        ItemTemplate const* proto = item->GetTemplate();
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        if (Collectable(proto, spell, kind) && !player->HasSpell(spell) && player->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            player->learnSpell(spell);
            ChatHandler(player->GetSession()).PSendSysMessage("You learn {}. The spare is still yours to trade.", Bracketed(proto));
        }
    }

    // Once per account: the core's mail has no item for these (StripAchievementRewards).
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        // Bots too: the mail has no item for anyone now, and a player's alt that a bot plays
        // earns it for the player's account.
        if (!collectionConfig.enabled || !collectionConfig.achievements || !player->GetSession())
            return;

        auto itr = achievementItems.find(achievement->ID);
        if (itr == achievementItems.end())
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itr->second);
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        if (!Teaches(proto, spell, kind))
            return;

        uint32 account = player->GetSession()->GetAccountId();
        bool first;
        {
            std::lock_guard<std::mutex> guard(achievementsLock);
            first = achievementsGiven.insert(Key(account, achievement->ID)).second;
        }

        if (first)
            CharacterDatabase.Execute("INSERT IGNORE INTO mod_boss_coins_collection_achievement (account, achievement, item, guid) "
                "VALUES ({}, {}, {}, {})", account, achievement->ID, proto->ItemId, player->GetGUID().GetCounter());

        if (AccountHas(player, spell, kind))
        {
            if (IsRealPlayer(player))
                ChatHandler(player->GetSession()).PSendSysMessage(first ? "Your account has {} already." :
                "Your account got {} for this achievement already.", Bracketed(proto));
            return;
        }

        // Only ever the spell, never an item: an account that lost it (its last character was
        // deleted, and the sharing module forgot its spells) gets it back, and nothing is gained
        // by earning the achievement again.
        GiveAccount(player, proto, spell, kind, nullptr);
    }
};

// Right-clicking a soulbound mount or companion item: caught before the core checks what the
// item asks of the character, so a character too low for it can still collect it or trade a
// spare. Runs on the thread that handles the player's packets, right before the handler.
class BossCoinsCollectionServerScript : public ServerScript
{
public:
    BossCoinsCollectionServerScript() : ServerScript("BossCoinsCollectionServerScript", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        // bag, slot, cast count, spell, item guid, ...
        if (packet.GetOpcode() != CMSG_USE_ITEM || !collectionConfig.enabled || packet.size() < 15)
            return true;

        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld() || !IsRealPlayer(player) || player->m_mover != player)
            return true;

        Item* item = player->GetUseableItemByPos(packet.read<uint8>(0), packet.read<uint8>(1));
        if (!item || item->GetGUID().GetRawValue() != packet.read<uint64>(7) || item->IsInTrade())
            return true;

        ItemTemplate const* proto = item->GetTemplate();
        uint32 spell = 0;
        uint8 kind = ACCOUNT_NONE;
        if (!Collectable(proto, spell, kind))
            return true;

        LastUse* lastUse = player->CustomData.GetDefault<LastUse>(LAST_USE_KEY);
        uint64 now = NowMs();
        if (lastUse->ms && now - lastUse->ms < USE_INTERVAL_MS)
        {
            player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);
            return false;
        }
        lastUse->ms = now;

        if (!AccountHas(player, spell, kind))
        {
            GiveAccount(player, proto, spell, kind, item);
            return false;
        }

        // Not tradeable: the core says what it always says (already known, or it teaches it).
        uint32 coins = ExchangeCoins(proto, kind);
        if (!coins)
            return true;

        // Frees the item the client greyed out while it waited for an answer.
        player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);
        ShowExchange(player, item, kind, coins);
        return false;
    }
};

void AddBossCoinsCollectionScripts()
{
    new BossCoinsCollectionWorldScript();
    new BossCoinsCollectionPlayerScript();
    new BossCoinsCollectionServerScript();
}
