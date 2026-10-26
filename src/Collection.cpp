/*
 * mod-boss-coins: mounts and companions belong to the account.
 *
 * On a server that shares mounts and companions between an account's characters
 * (mod-accountwide-mounts, mod-accountwide-pets or warblups/mod-accountwide, with their tables
 * accountwide_mounts / accountwide_pets), the item that teaches one is only a way to the spell.
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
 *     which deleting characters doesn't touch, so a new alt can't earn it again for a spare.
 *   - A spare from a drop is worth coins: right-click one the account has already and a box
 *     offers to trade it for Tarnished Undermine Reals, paid into the account wallet. Only for
 *     items that drop (a creature, a chest, fishing, a container nobody sells) and that no vendor
 *     sells, no quest gives, no achievement gives and the launcher's shop doesn't sell: anything
 *     else would turn gold, or a quest done again on an alt, into coins. How many: by quality
 *     (BossCoins.Collection.Exchange.*), or world table mod_boss_coins_exchange per item.
 *
 * Bind on equip and bind on use items are left alone: they can still be sold. Bots are skipped.
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
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace BossCoins;

namespace
{
    // What an item's spell is: a mount (the Mounts skill line) or a companion (the Companions
    // line), each with the table its sharing module keeps per account.
    enum CollectionKind : uint8
    {
        KIND_NONE   = 0,
        KIND_MOUNTS = 1,
        KIND_PETS   = 2,
    };

    constexpr char const* KIND_TABLES[] = { "", "accountwide_mounts", "accountwide_pets" };

    // Item spells that teach the spell in the item's second spell slot. Mount and companion items
    // use one of these two.
    constexpr int32 SPELL_LEARNING = 483;
    constexpr int32 SPELL_LEARNING_COMPANION = 55884;

    // The trade-a-spare box: its gossip menu id, which is also its npc_text (the module's world
    // SQL), from Pix Xizzix's range.
    constexpr uint32 MENU_EXCHANGE = 9503210;

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
    };
    std::unordered_map<uint32, OriginalReward> originalRewards;

    // (account << 32 | achievement) of every achievement mount or companion given. Achievements
    // complete on the map threads.
    std::mutex achievementsLock;
    std::unordered_set<uint64> achievementsGiven;

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

    uint8 KindOf(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            if (itr->second->SkillLine == SKILL_MOUNTS)
                return KIND_MOUNTS;
            if (itr->second->SkillLine == SKILL_COMPANIONS)
                return KIND_PETS;
        }
        return KIND_NONE;
    }

    // The spell an item teaches and what it is, when it teaches a mount or companion that the
    // server shares between characters.
    bool Teaches(ItemTemplate const* proto, uint32& spell, uint8& kind)
    {
        if (!proto || (proto->Spells[0].SpellId != SPELL_LEARNING && proto->Spells[0].SpellId != SPELL_LEARNING_COMPANION))
            return false;

        if (proto->Spells[1].SpellId <= 0 || !sSpellMgr->GetSpellInfo(proto->Spells[1].SpellId))
            return false;

        spell = uint32(proto->Spells[1].SpellId);
        kind = KindOf(spell);
        return kind != KIND_NONE && kindTables[kind];
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

        if (auto itr = exchangeOverrides.find(proto->ItemId); itr != exchangeOverrides.end())
            return itr->second;

        if (!dropItems.count(proto->ItemId))
            return 0;

        if (proto->Quality >= ITEM_QUALITY_EPIC)
            return kind == KIND_MOUNTS ? collectionConfig.coinsEpicMount : collectionConfig.coinsEpicCompanion;
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
        if (CharacterDatabase.Query("SELECT 1 FROM `{}` WHERE `account_id` = {} AND `spell` = {} LIMIT 1",
            KIND_TABLES[kind], account, spell))
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
        trans->Append("INSERT IGNORE INTO `{}` (`account_id`, `spell`) VALUES ({}, {})", KIND_TABLES[kind], account, spell);
        if (item)
        {
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
            player->SaveInventoryAndGoldToDB(trans);
        }
        CharacterDatabase.CommitTransaction(trans);

        ChatHandler chat(player->GetSession());
        if (!player->HasSpell(spell) && player->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            player->learnSpell(spell);
            chat.PSendSysMessage("{} is in your account's collection now: you know it, and your other characters learn it when they log in.",
                Bracketed(proto));
        }
        else
            chat.PSendSysMessage("{} is in your account's collection now: your characters learn it when they log in, each once it can use it.",
                Bracketed(proto));

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
            fmt::format("Trade your spare {} for {} {}? The coins go to your account wallet. Your account keeps the {} itself.",
                proto->Name1, coins, CoinName(coins), kind == KIND_MOUNTS ? "mount" : "companion"),
            0, false);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Keep it.", 0, 0);
        SendGossipMenuFor(player, MENU_EXCHANGE, player->GetGUID());
    }

    void Exchange(Player* player, Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        uint32 spell = 0;
        uint8 kind = KIND_NONE;
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
        uint8 kind = KIND_NONE;
        if (!Collectable(proto, spell, kind))
            return;

        if (!AccountHas(player, spell, kind))
        {
            GiveAccount(player, proto, spell, kind, item);
            return;
        }

        ChatHandler chat(player->GetSession());
        if (uint32 coins = ExchangeCoins(proto, kind))
            chat.PSendSysMessage("Your account has {} already. Right-click it to trade it for {} {}.", Bracketed(proto), coins, CoinName(coins));
        else
            chat.PSendSysMessage("Your account has {} already.", Bracketed(proto));
    }

    // Item ids as a list for an IN ().
    std::string IdList(std::unordered_set<uint32> const& ids)
    {
        std::string list;
        for (uint32 id : ids)
            list += (list.empty() ? "" : ",") + std::to_string(id);
        return list;
    }

    // Items whose spares may be traded: the soulbound mount and companion items that drop, less
    // those that can be had another way.
    void LoadDropItems()
    {
        dropItems.clear();

        std::unordered_set<uint32> candidates;
        for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        {
            uint32 spell = 0;
            uint8 kind = KIND_NONE;
            if (Collectable(&proto, spell, kind))
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

        std::unordered_set<uint32> drops;
        for (char const* table : { "creature_loot_template", "gameobject_loot_template", "fishing_loot_template",
            "skinning_loot_template", "pickpocketing_loot_template", "reference_loot_template" })
            collect(fmt::format("SELECT DISTINCT Item FROM {} WHERE Reference = 0 AND Item IN ({})", table, ids), drops);

        // A container counts when no vendor sells it (a holiday boss's satchel does, a box bought
        // for gold doesn't).
        collect(fmt::format("SELECT DISTINCT l.Item FROM item_loot_template l WHERE l.Reference = 0 AND l.Item IN ({}) "
            "AND l.Entry NOT IN (SELECT item FROM npc_vendor) AND l.Entry NOT IN (SELECT item FROM game_event_npc_vendor)", ids), drops);

        std::unordered_set<uint32> other;
        collect(fmt::format("SELECT DISTINCT item FROM npc_vendor WHERE item IN ({})", ids), other);
        collect(fmt::format("SELECT DISTINCT item FROM game_event_npc_vendor WHERE item IN ({})", ids), other);
        collect(fmt::format("SELECT DISTINCT item FROM mod_boss_coins_shop WHERE item IN ({})", ids), other);
        for (char const* column : { "RewardItem1", "RewardItem2", "RewardItem3", "RewardItem4", "RewardChoiceItemID1",
            "RewardChoiceItemID2", "RewardChoiceItemID3", "RewardChoiceItemID4", "RewardChoiceItemID5", "RewardChoiceItemID6" })
            collect(fmt::format("SELECT DISTINCT {0} FROM quest_template WHERE {0} IN ({1})", column, ids), other);
        // The table, not the core's copy: the module takes the items out of that (see
        // StripAchievementRewards).
        collect(fmt::format("SELECT DISTINCT ItemID FROM achievement_reward WHERE ItemID IN ({})", ids), other);

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
    // are given apart from the mail and stay), and put back when the setting is off. Only on the
    // world thread (startup, .reload config), when no map is being updated.
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
            if (original == originalRewards.end())
            {
                uint32 spell = 0;
                uint8 kind = KIND_NONE;
                if (!reward->itemId || !Teaches(sObjectMgr->GetItemTemplate(reward->itemId), spell, kind))
                    continue;

                original = originalRewards.emplace(id, OriginalReward{ reward->itemId, reward->sender }).first;
                achievementItems[id] = reward->itemId;
            }

            reward->itemId = strip ? 0 : original->second.itemId;
            reward->sender = strip ? 0 : original->second.sender;
        }
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
    for (uint8 kind = KIND_MOUNTS; kind <= KIND_PETS; ++kind)
    {
        // In a LIKE pattern an underscore matches any one character: escaped, so only the table
        // itself counts.
        std::string pattern;
        for (char c : std::string_view(KIND_TABLES[kind]))
            pattern += c == '_' ? "\\_" : std::string(1, c);
        kindTables[kind] = CharacterDatabase.Query("SHOW TABLES LIKE '{}'", pattern) != nullptr;
    }

    if (!kindTables[KIND_MOUNTS] && !kindTables[KIND_PETS])
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
        "spares, {} come from achievements", kindTables[KIND_MOUNTS] ? "mounts" : "",
        kindTables[KIND_MOUNTS] && kindTables[KIND_PETS] ? " and companions" : kindTables[KIND_PETS] ? "companions" : "",
        dropItems.size(), achievementItems.size());
}

class BossCoinsCollectionWorldScript : public WorldScript
{
public:
    BossCoinsCollectionWorldScript() : WorldScript("BossCoinsCollectionWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        collectionConfig.enabled = sConfigMgr->GetOption<bool>("BossCoins.Collection.Enable", true);
        collectionConfig.achievements = sConfigMgr->GetOption<bool>("BossCoins.Collection.Achievements", true);
        collectionConfig.coinsCommon = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.Common", 2);
        collectionConfig.coinsRare = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.Rare", 5);
        collectionConfig.coinsEpicCompanion = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.EpicCompanion", 15);
        collectionConfig.coinsEpicMount = sConfigMgr->GetOption<uint32>("BossCoins.Collection.Exchange.EpicMount", 25);

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
        uint8 kind = KIND_NONE;
        if (!item || !Collectable(item->GetTemplate(), spell, kind) || !IsRealPlayer(player))
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
        --playersPending;

        for (ObjectGuid const& guid : items)
            if (Item* item = player->GetItemByGuid(guid); item && !player->IsBankPos(item->GetPos()) && !item->IsInTrade())
                Collect(player, item);
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
        if (!item || item->GetEntry() != sender || player->IsBankPos(item->GetPos()) || item->IsInTrade())
            return;

        Exchange(player, item);
    }

    // Once per account: the core's mail has no item for these (StripAchievementRewards).
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (!collectionConfig.enabled || !collectionConfig.achievements || !IsRealPlayer(player))
            return;

        auto itr = achievementItems.find(achievement->ID);
        if (itr == achievementItems.end())
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itr->second);
        uint32 spell = 0;
        uint8 kind = KIND_NONE;
        if (!Teaches(proto, spell, kind))
            return;

        uint32 account = player->GetSession()->GetAccountId();
        {
            std::lock_guard<std::mutex> guard(achievementsLock);
            if (!achievementsGiven.insert(Key(account, achievement->ID)).second)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Your account got {} for this achievement already.", Bracketed(proto));
                return;
            }
        }

        CharacterDatabase.Execute("INSERT IGNORE INTO mod_boss_coins_collection_achievement (account, achievement, item, guid) "
            "VALUES ({}, {}, {}, {})", account, achievement->ID, proto->ItemId, player->GetGUID().GetCounter());

        if (AccountHas(player, spell, kind))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Your account has {} already.", Bracketed(proto));
            return;
        }

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
        uint8 kind = KIND_NONE;
        if (!Collectable(proto, spell, kind))
            return true;

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
