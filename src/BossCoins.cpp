/*
 * mod-boss-coins: Tarnished Undermine Reals.
 *
 * A boss that dies gives every real player in its instance a few coins (item 90410), straight
 * into their bags: no loot roll, and nothing for whoever opens the corpse to forget. The coins
 * are spent at Pix Xizzix in Booty Bay, who is data: the module's SQL. This file is the giving.
 *
 *   which bosses    world table mod_boss_coins_source: a row per instance map, or per boss
 *   what a boss is  whatever the core credits as a dungeon encounter (instance_encounters), so
 *                   scripted endings count too: Majordomo Executus surrendering, the Ring of Law
 *   how often       once per character per boss until the next daily quest reset. A dungeon can
 *                   be reset and run again; its bosses pay once a day all the same
 *   who             players with a client. Bots are sessions without one and get nothing
 *
 * The "once" is kept in memory (and in characters table mod_boss_coins_award, so a restart
 * doesn't forget it): the memory is what is checked, the table is only read at startup.
 *
 * Released under the MIT License.
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
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
    // A row older than this can't matter any more: the daily reset is at most a day back.
    constexpr uint32 AWARD_KEPT_SECONDS = 2 * DAY;

    struct Config
    {
        bool enabled = true;
        uint32 minBossLevel = 55;
    };

    Config config;

    // mod_boss_coins_source, read once at startup: (map, encounter) -> coins, encounter 0 for
    // "every boss of the map". Never written after that, so map threads read it freely.
    std::unordered_map<uint64, uint32> sources;

    // When each character last got a boss's coins: character guid -> encounter -> time. Bosses
    // die on map threads, several maps at once, hence the lock.
    std::mutex awardsLock;
    std::unordered_map<uint32, std::unordered_map<uint32, time_t>> awards;

    uint64 SourceKey(uint32 mapId, uint32 encounterId)
    {
        return (uint64(mapId) << 32) | encounterId;
    }

    uint32 CoinsFor(uint32 mapId, uint32 encounterId)
    {
        auto itr = sources.find(SourceKey(mapId, encounterId));
        if (itr == sources.end())
            itr = sources.find(SourceKey(mapId, 0));

        return itr != sources.end() ? itr->second : 0;
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

    // True if the character hasn't had this boss's coins since the last daily reset, and
    // remembers that it has them now.
    bool ClaimForToday(Player* player, uint32 encounterId)
    {
        time_t now = GameTime::GetGameTime().count();
        time_t lastReset = time_t(sWorld->GetNextDailyQuestsResetTime().count()) - DAY;
        uint32 guid = player->GetGUID().GetCounter();

        {
            std::lock_guard<std::mutex> guard(awardsLock);
            time_t& last = awards[guid][encounterId];
            if (last >= lastReset)
                return false;

            last = now;
        }

        CharacterDatabase.Execute("REPLACE INTO mod_boss_coins_award (guid, encounter, awarded) VALUES ({}, {}, {})",
            guid, encounterId, uint64(now));
        return true;
    }

    void GiveCoins(Player* player, uint32 count)
    {
        ItemTemplate const* coin = sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL);
        if (!coin)
            return;

        ChatHandler chat(player->GetSession());

        // A character can own only so many (bank included). What doesn't fit under that is gone:
        // mailing it would only park it in a mailbox it can't be taken out of.
        if (coin->MaxCount > 0)
        {
            uint32 owned = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
            uint32 room = owned < uint32(coin->MaxCount) ? uint32(coin->MaxCount) - owned : 0;
            if (count > room)
            {
                chat.PSendSysMessage("You can't carry more than {} Tarnished Undermine Reals. Spend some at Pix Xizzix in Booty Bay.",
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

    void LoadSources()
    {
        sources.clear();

        if (QueryResult result = WorldDatabase.Query("SELECT map, encounter, coins FROM mod_boss_coins_source"))
        {
            do
            {
                Field* fields = result->Fetch();
                if (uint32 coins = fields[2].Get<uint8>())
                    sources[SourceKey(fields[0].Get<uint16>(), fields[1].Get<uint32>())] = coins;
            } while (result->NextRow());
        }

        if (!sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL))
            LOG_ERROR("server.loading", "mod-boss-coins: item {} is missing. The module's world SQL hasn't been applied; no coins will be given.",
                ITEM_TARNISHED_UNDERMINE_REAL);

        LOG_INFO("server.loading", "mod-boss-coins: {} coin source(s) loaded", sources.size());
    }

    void LoadAwards()
    {
        uint64 oldest = uint64(GameTime::GetGameTime().count()) - AWARD_KEPT_SECONDS;
        CharacterDatabase.DirectExecute("DELETE FROM mod_boss_coins_award WHERE awarded < {}", oldest);

        std::lock_guard<std::mutex> guard(awardsLock);
        awards.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, encounter, awarded FROM mod_boss_coins_award"))
        {
            do
            {
                Field* fields = result->Fetch();
                awards[fields[0].Get<uint32>()][fields[1].Get<uint32>()] = time_t(fields[2].Get<uint64>());
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

            uint32 encounterId = encounter->dbcEntry->id;
            uint32 coins = CoinsFor(map->GetId(), encounterId);
            if (!coins)
                continue;

            // The early bosses of a long dungeon are below the level the coins are for.
            Creature* boss = source ? source->ToCreature() : nullptr;
            if (boss && boss->GetLevel() < config.minBossLevel)
                return;

            for (MapReference const& ref : map->GetPlayers())
                if (Player* player = ref.GetSource())
                    if (IsRealPlayer(player) && ClaimForToday(player, encounterId))
                        GiveCoins(player, coins);

            return;
        }
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
    new BossCoinsWorldScript();
}
