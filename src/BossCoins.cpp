/*
 * mod-boss-coins: Tarnished Undermine Reals.
 *
 * A boss that dies gives every real player in its instance a few coins (item 90410), straight
 * into their bags: no loot roll, and nothing for whoever opens the corpse to forget. The coins
 * are spent at Pix Xizzix in Booty Bay, who is data: the module's SQL. This file is the giving,
 * and one check on the buying that the data can't make.
 *
 *   which bosses    world table mod_boss_coins_source: a row per instance map, or per boss
 *   what a boss is  whatever the core credits as a dungeon encounter (instance_encounters), so
 *                   scripted endings count too, like Majordomo Executus surrendering.
 *                   Where the core's credit doesn't cover a boss, world table
 *                   mod_boss_coins_credit names the creatures that are it: the four that take
 *                   turns at Zul'Gurub's Edge of Madness, and the Four Horsemen of the 40 player
 *                   Naxxramas. Such a boss pays when the last of its creatures in the instance
 *                   dies
 *   how often       once per character per boss and period. A raid's period is its lockout: it
 *                   ends when the raid next resets. A dungeon can be reset at will, so its
 *                   period ends at the next daily quest reset
 *   who             players with a client. Bots are sessions without one and get nothing
 *
 * The "once" is kept in memory (and in characters table mod_boss_coins_award, so a restart
 * doesn't forget it): the memory is what is checked, the table is only read at startup.
 *
 * Released under the MIT License.
 */

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
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <unordered_map>

namespace
{
    constexpr uint32 ITEM_TARNISHED_UNDERMINE_REAL = 90410;
    // Pix Xizzix, and the creature entries that own the pages of her stock (the module's SQL).
    constexpr uint32 NPC_PIX_XIZZIX = 9503200;
    constexpr uint32 NPC_PIX_XIZZIX_LAST_PAGE = 9503299;

    struct Config
    {
        bool enabled = true;
        uint32 minBossLevel = 55;
    };

    Config config;

    // mod_boss_coins_source, read once at startup: (map, encounter) -> coins, encounter 0 for
    // "every boss of the map". A row with 0 coins is kept: it is how one boss is switched off.
    // Never written after startup, so map threads read it freely.
    std::unordered_map<uint64, uint32> sources;

    // mod_boss_coins_credit, read once at startup like the sources: creature entry -> the
    // encounter it is (part of).
    std::unordered_map<uint32, uint32> extraCredits;

    // The period each character last got a boss's coins in, as the time that period ends:
    // character guid -> encounter -> period end. Bosses die on map threads, several maps at
    // once, hence the lock.
    std::mutex awardsLock;
    std::unordered_map<uint32, std::unordered_map<uint32, uint64>> awards;

    uint64 SourceKey(uint32 mapId, uint32 encounterId)
    {
        return (uint64(mapId) << 32) | encounterId;
    }

    // Bot sessions: IsHeadless() on current playerbots cores, IsBot() on older ones.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !IsBotSession(session);
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

    // True if the character hasn't had this boss's coins in this period, and remembers that it
    // has them now. Periods are compared for being the same one, not for order: a reset time
    // that an admin moves must never lock a boss for longer than its new period.
    bool ClaimForPeriod(Player* player, uint32 encounterId, uint64 periodEnd)
    {
        uint32 guid = player->GetGUID().GetCounter();

        {
            std::lock_guard<std::mutex> guard(awardsLock);
            uint64& last = awards[guid][encounterId];
            if (last == periodEnd)
                return false;

            last = periodEnd;
        }

        CharacterDatabase.Execute("REPLACE INTO mod_boss_coins_award (guid, encounter, period_end) VALUES ({}, {}, {})",
            guid, encounterId, periodEnd);
        return true;
    }

    void GiveCoins(Player* player, uint32 count)
    {
        ItemTemplate const* coin = sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL);
        if (!coin)
            return;

        ChatHandler chat(player->GetSession());

        // A character can own only so many, counting the bank and what is waiting in the mail.
        // What doesn't fit under that is gone.
        if (coin->MaxCount > 0)
        {
            uint32 owned = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
            for (auto const& [itemGuid, mailed] : player->mMitems)
                if (mailed && mailed->GetEntry() == ITEM_TARNISHED_UNDERMINE_REAL)
                    owned += mailed->GetCount();

            uint32 room = owned < uint32(coin->MaxCount) ? uint32(coin->MaxCount) - owned : 0;
            if (count > room)
            {
                chat.PSendSysMessage("You can't have more than {} Tarnished Undermine Reals. Spend some at Pix Xizzix in Booty Bay.",
                    coin->MaxCount);
                count = room;
            }
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

        uint64 periodEnd = PeriodEnd(map);
        for (MapReference const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
                if (IsRealPlayer(player) && ClaimForPeriod(player, encounterId, periodEnd))
                    GiveCoins(player, itr->second);
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
        // A period that ended can't be the current one again. The day's margin is for a start
        // right after a reset time has passed and before the core has moved on to the next.
        uint64 now = uint64(GameTime::GetGameTime().count());
        CharacterDatabase.DirectExecute("DELETE FROM mod_boss_coins_award WHERE period_end < {}", now > DAY ? now - DAY : 0);

        std::lock_guard<std::mutex> guard(awardsLock);
        awards.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, encounter, period_end FROM mod_boss_coins_award"))
        {
            do
            {
                Field* fields = result->Fetch();
                awards[fields[0].Get<uint32>()][fields[1].Get<uint32>()] = fields[2].Get<uint64>();
            } while (result->NextRow());
        }
    }
}

class BossCoinsGlobalScript : public GlobalScript
{
public:
    BossCoinsGlobalScript() : GlobalScript("BossCoinsGlobalScript", { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE }) { }

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

        // The encounter has to be one of this instance's: a creature of that entry spawned
        // anywhere else is not the boss.
        Map* map = unit->FindMap();
        DungeonEncounterEntry const* encounter = sDungeonEncounterStore.LookupEntry(itr->second);
        if (!map || !map->IsDungeon() || !encounter || encounter->mapId != map->GetId())
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
    BossCoinsPlayerScript() : PlayerScript("BossCoinsPlayerScript", { PLAYERHOOK_ON_BEFORE_BUY_ITEM_FROM_VENDOR }) { }

    // The core sells from the vendor list the session last opened, and checks the conditions of
    // whichever creature the buy packet names. It never asks whether that creature offers that
    // list to this player. For Pix Xizzix's pages it has to: the list a session remembers
    // survives a switch to another character, and a made-up packet can name any vendor. So a
    // purchase from one of her pages must name her, and she must offer the page right now.
    void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid vendorguid, uint32 /*vendorslot*/, uint32& item,
        uint8 /*count*/, uint8 /*bag*/, uint8 /*slot*/) override
    {
        uint32 page = player->GetSession()->GetCurrentVendor();
        if (page <= NPC_PIX_XIZZIX || page > NPC_PIX_XIZZIX_LAST_PAGE)
            return;

        Creature* pix = player->GetNPCIfCanInteractWith(vendorguid, UNIT_NPC_FLAG_VENDOR);
        if (pix && pix->GetEntry() == NPC_PIX_XIZZIX && OffersPage(player, pix, page))
            return;

        player->SendBuyError(BUY_ERR_CANT_FIND_ITEM, nullptr, item, 0);
        item = 0;   // the core stops the purchase
    }
};

class BossCoinsWorldScript : public WorldScript
{
public:
    BossCoinsWorldScript() : WorldScript("BossCoinsWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.enabled = sConfigMgr->GetOption<bool>("BossCoins.Enable", true);
        config.minBossLevel = sConfigMgr->GetOption<uint32>("BossCoins.MinBossLevel", 55);

        LOG_INFO("server.loading", "mod-boss-coins: {}, bosses of level {} and up", config.enabled ? "enabled" : "disabled",
            config.minBossLevel);
    }

    void OnStartup() override
    {
        LoadSources();
        LoadAwards();
    }
};

void AddBossCoinsScripts()
{
    new BossCoinsGlobalScript();
    new BossCoinsUnitScript();
    new BossCoinsPlayerScript();
    new BossCoinsWorldScript();
}
