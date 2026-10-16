/*
 * mod-boss-coins: Tarnished Undermine Reals.
 *
 * A boss that dies gives every real player in its instance a few coins (item 90410), straight
 * into their account's wallet (CoinWallet.cpp), or with BossCoins.BossKills.ToWallet off into
 * their bags: no loot roll, and nothing for whoever opens the corpse to forget. The coins are
 * spent in the launcher's shop and at Pix Xizzix in Booty Bay, who is data: the module's SQL.
 * This file is the giving, and one check on the buying that the data can't make.
 *
 *   which bosses    world table mod_boss_coins_source: a row per instance map, or per boss
 *   what a boss is  whatever the core credits as a dungeon encounter (instance_encounters), so
 *                   scripted endings count too, like Majordomo Executus surrendering.
 *                   Where the core's credit doesn't fit a boss, world table
 *                   mod_boss_coins_credit names the creatures that are it, and that boss pays
 *                   by the table alone: when the last of its creatures in the instance dies.
 *                   The four that take turns at Zul'Gurub's Edge of Madness (the core credits
 *                   one), the bug trio of the Temple of Ahn'Qiraj (the core credits the first
 *                   death of one), the Four Horsemen of the 40 player Naxxramas
 *   how often       once per character per boss and period. A raid's period is its lockout: it
 *                   ends when the raid next resets. A dungeon can be reset at will, so its
 *                   period ends at the next daily quest reset. A raid boss also pays again in
 *                   another instance of the raid, which a character only gets into within one
 *                   lockout when a game master takes them off the first one
 *   who             players with a client. Bots are sessions without one and get nothing
 *
 * The "once" is kept in memory (and in characters table mod_boss_coins_award, so a restart
 * doesn't forget it): the memory is what is checked, the table is only read at startup.
 *
 * And a few coins a day for logging in: once per account per day, to the first character of the
 * account that is in the world after the daily quest reset. Per account, because the coins are
 * bound to the account: per character, every alt would be another helping. Kept the same way,
 * in characters table mod_boss_coins_daily. The daily coins go into the account's wallet
 * (CoinWallet.cpp), not into bags, unless BossCoins.DailyLogin.ToWallet is off.
 *
 * Released under the MIT License.
 */

#include "BossCoins.h"
#include "Chat.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "InstanceSaveMgr.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "TradeData.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace BossCoins;

bool BossCoins::IsRealPlayer(Player* player)
{
    WorldSession* session = player ? player->GetSession() : nullptr;
    return session && !IsBotSession(session);
}

char const* BossCoins::CoinName(uint32 count)
{
    return count == 1 ? "Tarnished Undermine Real" : "Tarnished Undermine Reals";
}

uint32 BossCoins::CoinRoom(Player* player)
{
    ItemTemplate const* coin = sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL);
    if (!coin)
        return 0;

    if (coin->MaxCount <= 0)
        return UINT32_MAX;

    uint32 owned = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
    for (auto const& [itemGuid, mailed] : player->mMitems)
        if (mailed && mailed->GetEntry() == ITEM_TARNISHED_UNDERMINE_REAL)
            owned += mailed->GetCount();

    return owned < uint32(coin->MaxCount) ? uint32(coin->MaxCount) - owned : 0;
}

namespace
{
    struct Config
    {
        bool enabled = true;
        uint32 minBossLevel = 55;
        uint32 dailyLoginCoins = 2;
        bool dailyToWallet = true;
        bool bossToWallet = true;
    };

    Config config;

    // mod_boss_coins_source, read once at startup: (map, encounter) -> coins, encounter 0 for
    // "every boss of the map". A row with 0 coins is kept: it is how one boss is switched off.
    // Never written after startup, so map threads read it freely.
    std::unordered_map<uint64, uint32> sources;

    // mod_boss_coins_credit, read once at startup like the sources: creature entry -> the
    // encounter it is (part of), and the encounters that have such creatures.
    std::unordered_map<uint32, uint32> extraCredits;
    std::unordered_set<uint32> creditEncounters;

    // What a character last got a boss's coins for: the period, as the time it ends, and for a
    // raid boss the instance it died in (0 for a dungeon boss).
    struct Award
    {
        uint64 periodEnd = 0;
        uint32 instanceId = 0;
    };

    // character guid -> encounter -> last award. Bosses die on map threads, several maps at
    // once, hence the lock.
    std::mutex awardsLock;
    std::unordered_map<uint32, std::unordered_map<uint32, Award>> awards;

    // account id -> the day it last got its daily coins, as the time the day ends. Real
    // players' logins and the world's update both run on the world thread (a bot's login can
    // come from a map thread, but stops before this), so the lock costs nothing; it is there
    // anyway.
    std::mutex dailyLock;
    std::unordered_map<uint32, uint64> dailyClaims;

    // The day the world's update last saw: when it changes, the daily quest reset has happened.
    uint64 dailySeen = 0;

    uint64 SourceKey(uint32 mapId, uint32 encounterId)
    {
        return (uint64(mapId) << 32) | encounterId;
    }

    // When the period a kill in this instance falls in ends. A raid: its next reset, so each
    // lockout pays once whatever hour the raid resets at. Anything else: the next daily quest
    // reset.
    uint64 PeriodEnd(Map* map)
    {
        if (map->IsRaid())
            if (time_t reset = sInstanceSaveMgr->GetResetTimeFor(map->GetId(), map->GetDifficulty()))
                return uint64(reset);

        return uint64(sWorld->GetNextDailyQuestsResetTime().count());
    }

    // The instance a kill counts for. A raid boss: the instance it died in. A character is
    // saved to it for the lockout, so the same lockout in another instance means a game master
    // released them, and the boss they can kill again pays again. A dungeon boss: none, or
    // resetting the dungeon would pay again.
    uint32 AwardInstance(Map* map)
    {
        return map->IsRaid() ? map->GetInstanceId() : 0;
    }

    // True if the character hasn't had this boss's coins in this period and instance, and
    // remembers that it has them now. Periods are compared for being the same one, not for
    // order: a reset time that an admin moves must never lock a boss for longer than its new
    // period.
    bool ClaimForPeriod(Player* player, uint32 encounterId, uint64 periodEnd, uint32 instanceId)
    {
        uint32 guid = player->GetGUID().GetCounter();

        {
            std::lock_guard<std::mutex> guard(awardsLock);
            Award& last = awards[guid][encounterId];
            if (last.periodEnd == periodEnd && last.instanceId == instanceId)
                return false;

            last = { periodEnd, instanceId };
        }

        CharacterDatabase.Execute("REPLACE INTO mod_boss_coins_award (guid, encounter, period_end, instance) VALUES ({}, {}, {}, {})",
            guid, encounterId, periodEnd, instanceId);
        return true;
    }

    // An instance is gone for good: nobody is saved to it any more. What was paid in it is
    // forgotten, because the server gives the id to a new instance after a restart, and that one
    // must not look like the old one.
    void ForgetInstance(uint32 instanceId)
    {
        if (!instanceId)
            return;

        bool forgot = false;
        {
            std::lock_guard<std::mutex> guard(awardsLock);
            for (auto& [guid, bosses] : awards)
                forgot |= std::erase_if(bosses, [instanceId](auto const& boss) { return boss.second.instanceId == instanceId; }) > 0;
        }

        // Every row of the table is in memory, so nothing forgotten means nothing to delete.
        if (forgot)
            CharacterDatabase.Execute("DELETE FROM mod_boss_coins_award WHERE instance = {}", instanceId);
    }

    void GiveCoins(Player* player, uint32 count)
    {
        ItemTemplate const* coin = sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL);
        if (!coin)
            return;

        ChatHandler chat(player->GetSession());

        // A character can own only so many. What doesn't fit under that is gone.
        uint32 room = CoinRoom(player);
        if (count > room)
        {
            chat.PSendSysMessage("You can't have more than {} Tarnished Undermine Reals. Spend some at Pix Xizzix in Booty Bay.",
                coin->MaxCount);
            count = room;
        }

        if (!count)
            return;

        uint32 noSpace = 0;
        ItemPosCountVec dest;
        InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_TARNISHED_UNDERMINE_REAL, count, &noSpace);
        uint32 stored = result == EQUIP_ERR_OK ? count : count - std::min(count, noSpace);

        if (stored && !dest.empty())
        {
            if (Item* item = player->StoreNewItem(dest, ITEM_TARNISHED_UNDERMINE_REAL, true))
                player->SendNewItem(item, stored, true, false);
            else
                stored = 0;
        }
        else
            stored = 0;

        // Full bags: the rest comes by mail, as the core does for quest rewards.
        if (stored < count)
        {
            player->SendItemRetrievalMail(ITEM_TARNISHED_UNDERMINE_REAL, count - stored);
            chat.SendSysMessage("Your bags are full. The Tarnished Undermine Reals that didn't fit were sent to your mailbox.");
        }
    }

    // Coins into the player's account wallet, which has no cap and no bags to fill. Returns the
    // balance after.
    uint32 ToWallet(Player* player, uint32 count, std::string_view reason)
    {
        uint32 balance = 0;
        Wallet::Add(player->GetSession()->GetAccountId(), player->GetGUID().GetCounter(), count, reason, 0, nullptr, &balance);
        return balance;
    }

    // The boss is dead: its coins to every real player in the instance who hasn't had them this
    // period. `source` is the creature the death or the core's credit came from.
    void PayBoss(Map* map, uint32 encounterId, Unit* source)
    {
        auto itr = sources.find(SourceKey(map->GetId(), encounterId));
        bool ownRow = itr != sources.end();
        if (!ownRow)
            itr = sources.find(SourceKey(map->GetId(), 0));

        if (itr == sources.end() || !itr->second)
            return;

        // The early bosses of a long dungeon are below the level the coins are for. A boss with
        // a row of its own was put there on purpose and pays whatever its level.
        if (!ownRow)
        {
            Creature* boss = source ? source->ToCreature() : nullptr;
            if (boss && boss->GetLevel() < config.minBossLevel)
                return;
        }

        // No coin item (the module's world SQL isn't applied): nothing to pay, into the bags or
        // the wallet.
        if (!sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL))
            return;

        // A raid boss that pays also saves the players to this instance, or it wouldn't be
        // theirs for the lockout and another one could pay them again. The core does it itself
        // for a boss that dies. Majordomo Executus surrenders.
        if (map->IsRaid())
            if (InstanceMap* instance = map->ToInstanceMap())
                instance->PermBindAllPlayers();

        uint64 periodEnd = PeriodEnd(map);
        uint32 instanceId = AwardInstance(map);
        for (MapReference const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
                if (IsRealPlayer(player) && ClaimForPeriod(player, encounterId, periodEnd, instanceId))
                {
                    if (config.bossToWallet)
                        ChatHandler(player->GetSession()).PSendSysMessage("{} {} into your account wallet (.wallet), which now holds {}.",
                            itr->second, CoinName(itr->second), ToWallet(player, itr->second, "boss"));
                    else
                        GiveCoins(player, itr->second);
                }
    }

    // The account's coins for the day, if nobody on it has had them yet. Into the account's
    // wallet; or, with the wallet switched off, into the bags, where a character that can't hold
    // them all leaves them for the account's next character in the world.
    void PayDaily(Player* player)
    {
        if (!config.enabled || !config.dailyLoginCoins || !IsRealPlayer(player))
            return;

        ItemTemplate const* coin = sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL);
        if (!coin)
            return;

        uint32 account = player->GetSession()->GetAccountId();
        uint64 dayEnd = uint64(sWorld->GetNextDailyQuestsResetTime().count());
        bool toWallet = config.dailyToWallet;

        {
            std::lock_guard<std::mutex> guard(dailyLock);
            uint64& last = dailyClaims[account];
            if (last == dayEnd)
                return;

            uint32 room = toWallet ? UINT32_MAX : CoinRoom(player);
            if (room < config.dailyLoginCoins)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Your daily {} {} {} waiting: this character already has {} of "
                    "the {} it can hold. Spend some at Pix Xizzix in Booty Bay, or log in another character.",
                    config.dailyLoginCoins, CoinName(config.dailyLoginCoins), config.dailyLoginCoins == 1 ? "is" : "are",
                    uint32(coin->MaxCount) - room, coin->MaxCount);
                return;
            }

            last = dayEnd;
        }

        CharacterDatabase.Execute("REPLACE INTO mod_boss_coins_daily (account, period_end) VALUES ({}, {})", account, dayEnd);

        if (toWallet)
        {
            uint32 balance = ToWallet(player, config.dailyLoginCoins, "daily");
            ChatHandler(player->GetSession()).PSendSysMessage("Daily login: {} {} added to your account wallet, which now holds {}. "
                "Spend them in the launcher's shop. Come back after the next daily reset for more.",
                config.dailyLoginCoins, CoinName(config.dailyLoginCoins), balance);
            return;
        }

        ChatHandler(player->GetSession()).PSendSysMessage("Daily login: {} {} for your account. Come back after the next daily reset "
            "for more.", config.dailyLoginCoins, CoinName(config.dailyLoginCoins));
        GiveCoins(player, config.dailyLoginCoins);
    }

    // Another creature of the same encounter is still standing in this instance. Only creatures
    // spawned from the creature table are seen, not ones a script summons.
    bool HasLivingPartner(Map* map, Unit* dead, uint32 encounterId)
    {
        for (auto const& [spawnId, creature] : map->GetCreatureBySpawnIdStore())
        {
            if (creature == dead || !creature->IsAlive())
                continue;

            auto itr = extraCredits.find(creature->GetEntry());
            if (itr != extraCredits.end() && itr->second == encounterId)
                return true;
        }

        return false;
    }

    // Pix Xizzix offers this page of her stock to this player: the option of her menu that opens
    // it exists and its conditions (progression phase, achievement) hold.
    bool OffersPage(Player* player, Creature* pix, uint32 page)
    {
        GossipMenuItemsMapBounds options = sObjectMgr->GetGossipMenuItemsMapBounds(pix->GetGossipMenuId());
        for (auto itr = options.first; itr != options.second; ++itr)
            if (itr->second.OptionType == GOSSIP_OPTION_VENDOR && itr->second.ActionMenuID == page)
                return sConditionMgr->IsObjectMeetToConditions(player, pix, itr->second.Conditions);

        return false;
    }

    void LoadSources()
    {
        sources.clear();
        extraCredits.clear();
        creditEncounters.clear();

        if (QueryResult result = WorldDatabase.Query("SELECT map, encounter, coins FROM mod_boss_coins_source"))
        {
            do
            {
                Field* fields = result->Fetch();
                sources[SourceKey(fields[0].Get<uint16>(), fields[1].Get<uint32>())] = fields[2].Get<uint8>();
            } while (result->NextRow());
        }

        uint32 skipped = 0;
        if (QueryResult result = WorldDatabase.Query("SELECT creature, encounter FROM mod_boss_coins_credit"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 creature = fields[0].Get<uint32>();
                uint32 encounterId = fields[1].Get<uint32>();
                // Rows for another module's bosses (the 40 player Naxxramas) on a server without it.
                if (!sObjectMgr->GetCreatureTemplate(creature) || !sDungeonEncounterStore.LookupEntry(encounterId))
                {
                    ++skipped;
                    continue;
                }

                extraCredits[creature] = encounterId;
                creditEncounters.insert(encounterId);
            } while (result->NextRow());
        }

        if (!sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL))
            LOG_ERROR("server.loading", "mod-boss-coins: item {} is missing. The module's world SQL hasn't been applied; no coins will be given.",
                ITEM_TARNISHED_UNDERMINE_REAL);

        LOG_INFO("server.loading", "mod-boss-coins: {} coin source(s) and {} boss creature(s) of its own loaded, {} skipped (not on this server)",
            sources.size(), extraCredits.size(), skipped);
    }

    void LoadAwards()
    {
        // A period that ended can't be the current one again. The day's margin costs nothing.
        uint64 now = uint64(GameTime::GetGameTime().count());
        CharacterDatabase.DirectExecute("DELETE FROM mod_boss_coins_award WHERE period_end < {}", now > DAY ? now - DAY : 0);

        // An instance the server dropped while it was starting up (one nobody is saved to any
        // more) never told the module, and its id is free for a new instance now.
        CharacterDatabase.DirectExecute("DELETE a FROM mod_boss_coins_award AS a LEFT JOIN instance AS i ON i.id = a.instance "
            "WHERE a.instance > 0 AND i.id IS NULL");

        std::lock_guard<std::mutex> guard(awardsLock);
        awards.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, encounter, period_end, instance FROM mod_boss_coins_award"))
        {
            do
            {
                Field* fields = result->Fetch();
                awards[fields[0].Get<uint32>()][fields[1].Get<uint32>()] = { fields[2].Get<uint64>(), fields[3].Get<uint32>() };
            } while (result->NextRow());
        }
    }

    void LoadDailyClaims()
    {
        // Only the current day's rows matter, and only to the end of it.
        uint64 now = uint64(GameTime::GetGameTime().count());
        CharacterDatabase.DirectExecute("DELETE FROM mod_boss_coins_daily WHERE period_end < {}", now > DAY ? now - DAY : 0);

        std::lock_guard<std::mutex> guard(dailyLock);
        dailyClaims.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT account, period_end FROM mod_boss_coins_daily"))
        {
            do
            {
                Field* fields = result->Fetch();
                dailyClaims[fields[0].Get<uint32>()] = fields[1].Get<uint64>();
            } while (result->NextRow());
        }
    }
}

class BossCoinsGlobalScript : public GlobalScript
{
public:
    BossCoinsGlobalScript() : GlobalScript("BossCoinsGlobalScript",
        { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE, GLOBALHOOK_ON_INSTANCEID_REMOVED }) { }

    void OnInstanceIdRemoved(uint32 instanceId) override
    {
        ForgetInstance(instanceId);
    }

    // Fires for every creature the core credits in an instance, bosses and others. `encounters`
    // is the instance's boss list for its difficulty; the credit is a boss if it is on it.
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32 creditEntry, Unit* source,
        Difficulty /*difficulty_fixed*/, DungeonEncounterList const* encounters, uint32 /*dungeonCompleted*/, bool /*updated*/) override
    {
        if (!config.enabled || !map || !encounters)
            return;

        for (DungeonEncounter const* encounter : *encounters)
        {
            if (encounter->creditType != type || encounter->creditEntry != creditEntry)
                continue;

            // A boss with creatures in mod_boss_coins_credit is paid from there, when the last
            // of them dies. The core's credit can come at the first death.
            if (!creditEncounters.count(encounter->dbcEntry->id))
                PayBoss(map, encounter->dbcEntry->id, source);

            return;
        }
    }
};

class BossCoinsUnitScript : public UnitScript
{
public:
    BossCoinsUnitScript() : UnitScript("BossCoinsUnitScript", true, { UNITHOOK_ON_UNIT_DEATH }) { }

    // Every death in the world comes through here, so the first question is the cheap one: is
    // this one of the handful of creatures in mod_boss_coins_credit?
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (extraCredits.empty() || !config.enabled || !unit->IsCreature())
            return;

        auto itr = extraCredits.find(unit->GetEntry());
        if (itr == extraCredits.end())
            return;

        // The encounter has to be one of this instance's, at its difficulty: a creature of that
        // entry spawned anywhere else is not the boss.
        Map* map = unit->FindMap();
        DungeonEncounterEntry const* encounter = sDungeonEncounterStore.LookupEntry(itr->second);
        if (!map || !map->IsDungeon() || !encounter || encounter->mapId != map->GetId()
            || encounter->difficulty != uint32(map->GetDifficulty()))
            return;

        // Killed by players, as the core asks before it credits a boss: not by a game master's
        // command or by something else in the room.
        if (!unit->ToCreature()->IsDamageEnoughForLootingAndReward())
            return;

        // A boss of several creatures isn't dead until they all are.
        if (HasLivingPartner(map, unit, encounter->id))
            return;

        PayBoss(map, encounter->id, unit);
    }
};

class BossCoinsPlayerScript : public PlayerScript
{
public:
    BossCoinsPlayerScript() : PlayerScript("BossCoinsPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR, PLAYERHOOK_ON_SEND_LIST_INVENTORY }) { }

    void OnPlayerLogin(Player* player) override
    {
        PayDaily(player);
    }

    // A 3.3.5 client draws a vendor's list once, with the item names it has in its cache at that
    // moment. A name it has to ask the server for arrives after the drawing, and nothing draws
    // the list again: on a client whose cache was just emptied the rows stay nameless until the
    // window is opened again. So before one of Pix Xizzix's pages goes out, the details of what
    // it will show this player go out first, the way the server answers the client's own
    // request for them. Fires at the start of WorldSession::SendListInventory.
    void OnPlayerSendListInventory(Player* player, ObjectGuid /*vendorGuid*/, uint32& vendorEntry) override
    {
        if (vendorEntry <= NPC_PIX_XIZZIX || vendorEntry > NPC_PIX_XIZZIX_LAST_PAGE)
            return;

        VendorItemData const* stock = sObjectMgr->GetNpcVendorItemList(vendorEntry);
        if (!stock)
            return;

        WorldSession* session = player->GetSession();
        auto sendDetails = [session](uint32 itemId)
        {
            WorldPacket request(CMSG_ITEM_QUERY_SINGLE, 4);
            request << uint32(itemId);
            session->HandleItemQuerySingleOpcode(request);
        };

        // the coin too: the window shows its icon beside every price
        sendDetails(ITEM_TARNISHED_UNDERMINE_REAL);

        for (uint8 slot = 0; slot < stock->GetItemCount(); ++slot)
        {
            VendorItem const* item = stock->GetItem(slot);
            if (!item)
                continue;

            // Only what the list will show: another class's pieces are held back by a condition
            // on her entry, which the core checks the same way.
            ConditionList conditions = sConditionMgr->GetConditionsForNpcVendorEvent(NPC_PIX_XIZZIX, item->item);
            if (sConditionMgr->IsObjectMeetToConditions(player, conditions))
                sendDetails(item->item);
        }
    }

    // The core sells from the vendor list the session last opened, and checks the conditions of
    // whichever creature the buy packet names. It never asks whether that creature offers that
    // list to this player. For Pix Xizzix's pages it has to: the list a session remembers
    // survives a switch to another character, and a made-up packet can name any vendor. So a
    // purchase from one of her pages must name her, and she must offer the page right now.
    void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid vendorguid, uint32 vendorslot, uint32& item,
        uint8 count, uint8 /*bag*/, uint8 /*slot*/) override
    {
        uint32 page = player->GetSession()->GetCurrentVendor();
        if (page <= NPC_PIX_XIZZIX || page > NPC_PIX_XIZZIX_LAST_PAGE)
            return;

        Creature* pix = player->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (!pix || pix->GetEntry() != NPC_PIX_XIZZIX || !OffersPage(player, pix, page))
        {
            player->SendBuyError(BUY_ERR_CANT_FIND_ITEM, nullptr, item, 0);
            item = 0;   // the core stops the purchase
            return;
        }

        PayShortfallFromWallet(player, pix, page, vendorslot, item, count);
    }

    // Her prices are in bag coins: the core checks the bags (not the bank, nor a trade window)
    // for the price right after this hook, and takes it from them. Coins in the account wallet
    // count too: when the bags are short, she moves what is missing out of the wallet into them
    // first, all of it or none. The core's cheap checks that come after the hook are made here
    // first, so a purchase that can't happen moves nothing. One can still fail after the move
    // (the coins took the slot the item needed): the moved coins then stay in the bags, and
    // the chat line says that is where they went.
    static void PayShortfallFromWallet(Player* player, Creature* pix, uint32 page, uint32 vendorslot, uint32 item,
        uint8 count)
    {
        VendorItemData const* stock = sObjectMgr->GetNpcVendorItemList(page);
        VendorItem const* stocked = stock ? stock->GetItem(vendorslot) : nullptr;
        if (!stocked || stocked->item != item || !stocked->ExtendedCost)
            return;

        ItemExtendedCostEntry const* cost = sItemExtendedCostStore.LookupEntry(stocked->ExtendedCost);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!cost || !proto)
            return;

        count = std::max<uint8>(count, 1);   // the core counts 0 as 1

        uint32 price = 0;
        for (uint8 i = 0; i < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++i)
            if (cost->reqitem[i] == ITEM_TARNISHED_UNDERMINE_REAL)
                price += cost->reqitemcount[i] * count;

        uint32 carried = CarriedForPrice(player);
        if (price <= carried)
            return;

        // The core's own checks, in its order, that would refuse the purchase anyway.
        if (!player->IsAlive())
            return;
        if (!(proto->AllowableClass & player->getClassMask()) && proto->Bonding == BIND_WHEN_PICKED_UP &&
            !player->IsGameMaster())
            return;
        ConditionList conditions = sConditionMgr->GetConditionsForNpcVendorEvent(pix->GetEntry(), item);
        if (!sConditionMgr->IsObjectMeetToConditions(player, pix, conditions))
            return;
        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item, proto->BuyCount * count) != EQUIP_ERR_OK)
            return;

        ChatHandler chat(player->GetSession());
        uint32 account = player->GetSession()->GetAccountId();
        uint32 missing = price - carried;
        uint32 balance = Wallet::Balance(account);
        if (balance < missing)
            return;   // the core's "missing items" says it

        if (!Wallet::ToBags(player, missing, "vendor", true))
        {
            if (CoinRoom(player) < missing)
                chat.PSendSysMessage("Your account wallet could pay the {} {} you are short, but a character can't own more "
                    "than {} Tarnished Undermine Reals, bank and mailbox included. Put some in the wallet first.",
                    missing, CoinName(missing), sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL)->MaxCount);
            else
                chat.PSendSysMessage("Your account wallet could pay the {} {} you are short, but your bags have no room "
                    "for them.", missing, CoinName(missing));
            return;
        }

        chat.PSendSysMessage("Pix Xizzix moves the {} {} you were short from your account wallet into your bags. "
            "The wallet holds {} now.", missing, CoinName(missing), Wallet::Balance(account));
    }

    // The coins the core counts for a price: those in the bags, less any in an open trade window
    // (HasItemCount skips items in trade).
    static uint32 CarriedForPrice(Player* player)
    {
        uint32 carried = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL);
        if (TradeData* trade = player->GetTradeData())
            for (uint8 slot = 0; slot < TRADE_SLOT_COUNT; ++slot)
                if (Item* traded = trade->GetItem(TradeSlots(slot)))
                    if (traded->GetEntry() == ITEM_TARNISHED_UNDERMINE_REAL && traded->IsInTrade())
                        carried -= std::min(carried, traded->GetCount());
        return carried;
    }
};

class BossCoinsWorldScript : public WorldScript
{
public:
    BossCoinsWorldScript() : WorldScript("BossCoinsWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("BossCoins.Enable", true);
        config.minBossLevel = sConfigMgr->GetOption<uint32>("BossCoins.MinBossLevel", 55);
        config.dailyLoginCoins = sConfigMgr->GetOption<uint32>("BossCoins.DailyLogin.Coins", 2);
        config.dailyToWallet = sConfigMgr->GetOption<bool>("BossCoins.DailyLogin.ToWallet", true);
        config.bossToWallet = sConfigMgr->GetOption<bool>("BossCoins.BossKills.ToWallet", true);

        LOG_INFO("server.loading", "mod-boss-coins: {}, bosses of level {} and up into the {}, {} coin(s) a day for logging in, "
            "into the {}", config.enabled ? "enabled" : "disabled", config.minBossLevel,
            config.bossToWallet ? "account wallet" : "bags", config.dailyLoginCoins,
            config.dailyToWallet ? "account wallet" : "bags");
    }

    void OnStartup() override
    {
        LoadSources();
        LoadAwards();
        LoadDailyClaims();
        Wallet::Load();
        Shop::Load();
        dailySeen = uint64(sWorld->GetNextDailyQuestsResetTime().count());
    }

    // Runs after the maps' update, on the world thread. Players who are in the world when the
    // daily quest reset happens get the new day's coins then, as if they had just logged in. So
    // does one on a loading screen: a far teleport takes the player out of the world for a while,
    // and the wallet or the bags take the coins all the same.
    void OnUpdate(uint32 /*diff*/) override
    {
        uint64 dayEnd = uint64(sWorld->GetNextDailyQuestsResetTime().count());
        if (dayEnd == dailySeen)
            return;

        dailySeen = dayEnd;
        for (auto const& [account, session] : sWorldSessionMgr->GetAllSessions())
            if (Player* player = session->GetPlayer())
                if (player->IsInWorld() || player->IsBeingTeleportedFar())
                    PayDaily(player);
    }
};

void AddBossCoinsScripts()
{
    new BossCoinsGlobalScript();
    new BossCoinsUnitScript();
    new BossCoinsPlayerScript();
    new BossCoinsWorldScript();
    AddBossCoinsWalletScripts();
    AddBossCoinsShopScripts();
}
