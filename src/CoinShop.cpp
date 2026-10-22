/*
 * mod-boss-coins: the launcher's shop.
 *
 * What the shop sells is world table mod_boss_coins_shop, a row per offer: an item that is
 * mailed to the character it is bought for (a mount, a companion), or a service that the
 * character gets at its next login (a new name, new looks, another race, the other faction).
 * Prices are in coins from the account wallet (CoinWallet.cpp). A price of 0 is a free offer:
 * the same rules (one per character, not for one that has it already), and no coins move.
 *
 * On a server that shares mounts and companions between an account's characters
 * (BossCoins.Shop.AccountWideSpells = 1, with mod-accountwide-mounts / mod-accountwide-pets or
 * warblups/mod-accountwide), a mount or companion is bought for the ACCOUNT: nothing is mailed,
 * its spell is written to those modules' table (accountwide_mounts / accountwide_pets, which
 * they read at every login) and every character of the account in the game learns it at once.
 * Such an offer is `account` in the listing, no character is `cannot`, and the account owns it
 * as soon as one character knows it. Without the table it is mailed as before.
 *
 * The launcher talks to the portal, and the portal to these two commands, over SOAP. They are
 * console commands: a player, a game master too, can't run them in game. Both answer one line
 * of JSON, errors included, so the portal never has to read a SOAP fault.
 *
 * .coinshop list <account id>
 *     {"ok":true,"balance":12,"phase":3,"characters":[{"guid","name","race","class","gender",
 *     "level","online"}],"offers":[{"id","category","name","description","icon","price","phase",
 *     "locked","unlock","account","owned":[guids],"cannot":[guids]}]}. `owned`: characters that
 *     have it already (an item in their bags, bank or mail, or the spell it teaches; a service
 *     waiting for their next login; an order still being written; every character for an
 *     `account` offer the account has). `cannot`: characters that can't use
 *     the item: race, class, level, or a skill it needs (riding, for a mount). The top `phase`
 *     is the account's: the mod-individual-progression phase of its furthest character. An
 *     offer with a higher `phase` is `locked` for every character, and `unlock` says what
 *     opens it, as a sentence ("Defeat Ragnaros in Molten Core with any character to unlock it.";
 *     past mod-individual-progression's ProgressionLimit "Comes to the realm with The Burning
 *     Crusade.").
 *
 * .coinshop buy <account id> <offer id> <character guid>
 *     {"ok":true,"balance":7,"message":"..."} or {"ok":false,"error":"<code>","message":"..."}.
 *     Codes: closed, no_offer, no_character, busy, locked, owned, cannot, balance. The coins are taken
 *     and the order written in one transaction; the message says why a refusal is one.
 *
 * .coinshop reload     reads mod_boss_coins_shop again (also for game masters in game)
 *
 * Released under the MIT License.
 */

#include "AsyncCallbackProcessor.h"
#include "BossCoins.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace BossCoins;
using namespace Acore::ChatCommands;

namespace
{
    enum ShopService : uint8
    {
        SERVICE_NONE           = 0,
        SERVICE_RENAME         = 1,
        SERVICE_CUSTOMIZE      = 2,
        SERVICE_CHANGE_RACE    = 3,
        SERVICE_CHANGE_FACTION = 4,
    };

    // What an item's spell is, for account-wide delivery: a mount (the Mounts skill line) or a
    // companion (the Companions line), each with the table its sharing module keeps per account.
    enum AccountKind : uint8
    {
        ACCOUNT_NONE   = 0,
        ACCOUNT_MOUNTS = 1,
        ACCOUNT_PETS   = 2,
    };

    // mod-accountwide-mounts and mod-accountwide-pets (and warblups/mod-accountwide before them):
    // (account_id, spell), read at every login. Looked for at Shop::Load; see accountTables.
    constexpr char const* ACCOUNT_TABLES[] = { "", "accountwide_mounts", "accountwide_pets" };

    struct Offer
    {
        uint32 id = 0;
        std::string category;
        std::string name;
        std::string description;
        std::string icon;
        uint32 price = 0;
        uint32 item = 0;
        uint32 count = 1;
        uint8 service = SERVICE_NONE;
        uint8 phase = 0;        // the progression phase the account must have reached
        uint32 teaches = 0;     // the spell the item teaches, if it is one that does
        uint8 accountKind = ACCOUNT_NONE;   // what `teaches` is, when the account can be given it
        // what the item asks of the character that uses it
        uint32 requiredLevel = 0;
        uint32 requiredSkill = 0;
        uint32 requiredSkillRank = 0;
    };

    struct Character
    {
        uint32 guid = 0;
        std::string name;
        uint8 race = 0;
        uint8 cls = 0;
        uint8 gender = 0;
        uint8 level = 0;
        uint32 atLogin = 0;
        Player* online = nullptr;
        std::unordered_map<uint32, uint16> skills;  // an offline character's, those the offers ask for
    };

    // How long a bought item waits in the mailbox, in days.
    constexpr uint32 SHOP_MAIL_DAYS = 180;

    struct ShopConfig
    {
        bool enabled = true;
        bool accountWideSpells = false;
        bool progressionGate = true;
        uint32 progressionLimit = 0;    // mod-individual-progression's own: no phase past it, 0 = none
    };

    // mod-individual-progression keeps a character's phase as rewarded hidden quests: 66000 +
    // phase, for every phase it has reached. Phases run from 0 (new character) to 18.
    constexpr uint32 PROGRESSION_QUEST = 66000;
    constexpr uint8 PROGRESSION_LAST_PHASE = 18;
    constexpr uint8 PROGRESSION_WOTLK_PHASE = 13;   // Kil'jaeden's kill opens Northrend; 8-12 are The Burning Crusade's

    // What takes a character into each phase, for the launcher's "locked" offers.
    char const* PhaseUnlock(uint8 phase)
    {
        switch (phase)
        {
            case 1:  return "Defeat Ragnaros in Molten Core";
            case 2:  return "Defeat Onyxia in Onyxia's Lair";
            case 3:  return "Defeat Nefarian in Blackwing Lair";
            case 4:  return "Bang the Scarab Gong";
            case 5:  return "Finish the quest Chaos and Destruction";
            case 6:  return "Defeat C'Thun in the Temple of Ahn'Qiraj";
            case 7:  return "Defeat Kel'Thuzad in Naxxramas (level 60)";
            case 8:  return "Finish the quest Into the Breach";
            case 9:  return "Defeat Prince Malchezaar in Karazhan";
            case 10: return "Defeat Kael'thas Sunstrider in Tempest Keep";
            case 11:
            case 12: return "Defeat Illidan Stormrage in the Black Temple";
            case 13: return "Defeat Kil'jaeden in the Sunwell Plateau";
            case 14: return "Defeat Kel'Thuzad in Naxxramas (level 80)";
            case 15: return "Defeat Yogg-Saron in Ulduar";
            case 16: return "Defeat Anub'arak in the Trial of the Crusader";
            case 17: return "Defeat the Lich King in Icecrown Citadel";
            case 18: return "Defeat Halion in the Ruby Sanctum";
            default: return "";
        }
    }

    ShopConfig shopConfig;

    // Whether the server has mod-individual-progression's phase quests. Without them nobody
    // ever reaches a phase, so the phases are off. Set by Shop::Load.
    bool progressionQuests = false;

    // Which of ACCOUNT_TABLES the characters database has, by AccountKind. Set by Shop::Load:
    // a write to a table that isn't there would abort the core.
    bool accountTables[3] = { false, false, false };

    // Read at startup and by .coinshop reload, both on the world thread, like every reader.
    std::vector<Offer> offers;

    // Orders that are paid for and not yet in the database, Key(offer, guid) -> how many. The
    // database's worker writes an order a moment after it is paid, and what an offline
    // character has is read from the database: a second order right behind the first (a double
    // click, a retry, the character logging out in between) would find nothing there yet and
    // pay again. An order counts as owned from the moment it is paid until its write is done;
    // from then on the database has it.
    std::unordered_map<uint64, uint32> ordersInFlight;

    // The orders' writes, until they are done. Asked on the world thread, every update.
    AsyncCallbackProcessor<TransactionCallback> orderCommits;

    // Services sold to characters that were offline, guid -> the order, until the character's
    // next login. A login reads the character's row once, at its start: one that read it before
    // the order's write landed brings the character into the world without the flag, and its
    // first save writes the row back without it. A bot of the player's own account logs in that
    // way too, unseen by the "busy" check. So at the login the flag is put back, if the row has
    // it by then (it doesn't when the service was used at the character list in between) or the
    // write hasn't landed yet. Logins can come from map threads (bots), hence the lock.
    struct ServiceOrder
    {
        uint32 flags = 0;
        uint32 inFlight = 0;
    };

    std::mutex serviceOrdersLock;
    std::unordered_map<uint32, ServiceOrder> serviceOrders;

    AtLoginFlags ServiceFlag(uint8 service)
    {
        switch (service)
        {
            case SERVICE_RENAME:         return AT_LOGIN_RENAME;
            case SERVICE_CUSTOMIZE:      return AT_LOGIN_CUSTOMIZE;
            case SERVICE_CHANGE_RACE:    return AT_LOGIN_CHANGE_RACE;
            case SERVICE_CHANGE_FACTION: return AT_LOGIN_CHANGE_FACTION;
            default:                     return AT_LOGIN_NONE;
        }
    }

    char const* ServiceName(uint8 service)
    {
        switch (service)
        {
            case SERVICE_RENAME:         return "Character Rename";
            case SERVICE_CUSTOMIZE:      return "Character Customization";
            case SERVICE_CHANGE_RACE:    return "Race Change";
            case SERVICE_CHANGE_FACTION: return "Faction Change";
            default:                     return "";
        }
    }

    Offer const* FindOffer(uint32 id)
    {
        auto itr = std::find_if(offers.begin(), offers.end(), [id](Offer const& offer) { return offer.id == id; });
        return itr != offers.end() ? &*itr : nullptr;
    }

    uint64 Key(uint32 offer, uint32 guid)
    {
        return (uint64(offer) << 32) | guid;
    }

    // Bought for the account rather than a character: a mount or companion, on a server that
    // shares them, with the sharing module's table to write it to.
    bool AccountWide(Offer const& offer)
    {
        return shopConfig.accountWideSpells && offer.accountKind != ACCOUNT_NONE && accountTables[offer.accountKind];
    }

    // A mount is a spell on the Mounts skill line, a companion one on the Companions line: what
    // puts them on the client's Mounts and Companions tabs, and what the sharing modules go by.
    uint8 AccountKindOf(uint32 spellId)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            if (itr->second->SkillLine == SKILL_MOUNTS)
                return ACCOUNT_MOUNTS;
            if (itr->second->SkillLine == SKILL_COMPANIONS)
                return ACCOUNT_PETS;
        }
        return ACCOUNT_NONE;
    }

    std::string IdList(std::vector<uint32> const& ids)
    {
        std::string list;
        for (uint32 id : ids)
            list += (list.empty() ? "" : ",") + std::to_string(id);
        return list;
    }

    // The account's characters, with the ones that are logged in (a loading screen counts).
    std::vector<Character> LoadCharacters(uint32 account)
    {
        std::vector<Character> characters;
        if (QueryResult result = CharacterDatabase.Query("SELECT guid, name, race, class, gender, level, at_login FROM characters "
            "WHERE account = {} ORDER BY guid", account))
        {
            do
            {
                Field* fields = result->Fetch();
                Character character;
                character.guid = fields[0].Get<uint32>();
                character.name = fields[1].Get<std::string>();
                character.race = fields[2].Get<uint8>();
                character.cls = fields[3].Get<uint8>();
                character.gender = fields[4].Get<uint8>();
                character.level = fields[5].Get<uint8>();
                character.atLogin = fields[6].Get<uint16>();
                character.online = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(character.guid));
                if (character.online)
                    character.level = character.online->GetLevel();
                characters.push_back(std::move(character));
            } while (result->NextRow());
        }

        return characters;
    }

    // The skills the offers ask for, of the account's offline characters. An online character
    // is asked directly.
    void LoadSkills(std::vector<Character>& characters, std::vector<Offer const*> const& wanted)
    {
        std::vector<uint32> guids, skills;
        for (Character const& character : characters)
            if (!character.online)
                guids.push_back(character.guid);
        for (Offer const* offer : wanted)
            if (offer->requiredSkill && std::find(skills.begin(), skills.end(), offer->requiredSkill) == skills.end())
                skills.push_back(offer->requiredSkill);

        if (guids.empty() || skills.empty())
            return;

        if (QueryResult result = CharacterDatabase.Query("SELECT guid, skill, value FROM character_skills WHERE guid IN ({}) AND skill IN ({})",
            IdList(guids), IdList(skills)))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 guid = fields[0].Get<uint32>();
                auto itr = std::find_if(characters.begin(), characters.end(), [guid](Character const& c) { return c.guid == guid; });
                if (itr != characters.end())
                    itr->skills[fields[1].Get<uint16>()] = fields[2].Get<uint16>();
            } while (result->NextRow());
        }
    }

    // Why the character can't use the offer's item: its race or class, its level, a skill it
    // lacks (riding, for a mount). Empty if it can. A mount bought for a character that can't
    // learn it would only sit in the bags.
    std::string CannotUse(Offer const& offer, Character const& character)
    {
        // An account-wide mount or companion is for every character: the sharing module decides
        // per character what it teaches and when (riding, level), nothing waits in anyone's bags.
        if (!offer.item || AccountWide(offer))
            return "";

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.item);
        if (!proto || !(proto->AllowableClass & (1u << (character.cls - 1))) || !(proto->AllowableRace & (1u << (character.race - 1))))
            return fmt::format("{} can't use the {}.", character.name, offer.name);

        if (character.level < offer.requiredLevel)
            return fmt::format("{} needs level {} for the {}.", character.name, offer.requiredLevel, offer.name);

        if (offer.requiredSkill)
        {
            uint16 value = 0;
            if (character.online)
                value = character.online->GetSkillValue(offer.requiredSkill);
            else if (auto itr = character.skills.find(offer.requiredSkill); itr != character.skills.end())
                value = itr->second;

            if (value < offer.requiredSkillRank)
            {
                SkillLineEntry const* skill = sSkillLineStore.LookupEntry(offer.requiredSkill);
                return fmt::format("{} needs {} ({}) for the {}.", character.name,
                    skill ? skill->name[LOCALE_enUS] : "a skill", offer.requiredSkillRank, offer.name);
            }
        }

        return "";
    }

    // The account's phase: that of its furthest character. A character in the game is asked
    // directly; the database has an online character's last save, not its now.
    uint8 AccountPhase(std::vector<Character> const& characters)
    {
        uint8 phase = 0;
        std::vector<uint32> offline;
        for (Character const& character : characters)
        {
            if (!character.online)
            {
                offline.push_back(character.guid);
                continue;
            }

            for (uint8 p = PROGRESSION_LAST_PHASE; p > phase; --p)
                if (character.online->GetQuestRewardStatus(PROGRESSION_QUEST + p))
                {
                    phase = p;
                    break;
                }
        }

        if (!offline.empty())
            if (QueryResult result = CharacterDatabase.Query("SELECT MAX(quest) FROM character_queststatus_rewarded "
                "WHERE guid IN ({}) AND quest BETWEEN {} AND {} AND active = 1",
                IdList(offline), PROGRESSION_QUEST + 1, PROGRESSION_QUEST + PROGRESSION_LAST_PHASE))
                if (!result->Fetch()[0].IsNull())
                    phase = std::max<uint8>(phase, result->Fetch()[0].Get<uint32>() - PROGRESSION_QUEST);

        return phase;
    }

    // Past the realm's progression limit an offer can't be opened yet: it waits for the realm to
    // move on to its expansion.
    bool PastLimit(Offer const& offer)
    {
        return shopConfig.progressionLimit && offer.phase > shopConfig.progressionLimit;
    }

    char const* Expansion(uint8 phase)
    {
        return phase >= PROGRESSION_WOTLK_PHASE ? "Wrath of the Lich King" : "The Burning Crusade";
    }

    // What opens a locked offer, as the sentence the launcher shows.
    std::string UnlockText(Offer const& offer)
    {
        if (PastLimit(offer))
            return fmt::format("Comes to the realm with {}.", Expansion(offer.phase));
        return fmt::format("{} with any character to unlock it.", PhaseUnlock(offer.phase));
    }

    bool Locked(Offer const& offer, uint8 accountPhase)
    {
        return shopConfig.progressionGate && progressionQuests && offer.phase > accountPhase;
    }

    // Which characters already have which offers, as Key(offer, guid). A character in the game
    // is asked directly; the database has an online character's last save, not its now.
    std::unordered_set<uint64> Owned(uint32 account, std::vector<Character> const& characters, std::vector<Offer const*> const& wanted)
    {
        std::unordered_set<uint64> owned;

        // The account's own mounts and companions, as the sharing modules have them: a spell
        // bought while nobody was in the game is there before any character knows it.
        std::unordered_set<uint32> accountSpells;
        for (uint8 kind = ACCOUNT_MOUNTS; kind <= ACCOUNT_PETS; ++kind)
        {
            std::vector<uint32> spells;
            for (Offer const* offer : wanted)
                if (offer->accountKind == kind && AccountWide(*offer))
                    spells.push_back(offer->teaches);
            if (spells.empty())
                continue;

            if (QueryResult result = CharacterDatabase.Query("SELECT spell FROM {} WHERE account_id = {} AND spell IN ({})",
                ACCOUNT_TABLES[kind], account, IdList(spells)))
            {
                do
                {
                    accountSpells.insert(result->Fetch()[0].Get<uint32>());
                } while (result->NextRow());
            }
        }

        std::vector<uint32> offline;
        for (Character const& character : characters)
            if (!character.online)
                offline.push_back(character.guid);

        std::vector<uint32> items, spells;
        for (Offer const* offer : wanted)
        {
            if (offer->item)
                items.push_back(offer->item);
            if (offer->teaches)
                spells.push_back(offer->teaches);
        }

        // offline characters: (guid, item) and (guid, spell) they have
        std::unordered_set<uint64> heldItems, knownSpells;
        if (!offline.empty() && !items.empty())
        {
            std::string guids = IdList(offline), entries = IdList(items);
            auto collect = [&heldItems](QueryResult result)
            {
                if (!result)
                    return;
                do
                {
                    Field* fields = result->Fetch();
                    heldItems.insert(Key(fields[1].Get<uint32>(), fields[0].Get<uint32>()));
                } while (result->NextRow());
            };

            collect(CharacterDatabase.Query("SELECT ci.guid, ii.itemEntry FROM character_inventory ci JOIN item_instance ii ON ii.guid = ci.item "
                "WHERE ci.guid IN ({}) AND ii.itemEntry IN ({})", guids, entries));
            collect(CharacterDatabase.Query("SELECT mi.receiver, ii.itemEntry FROM mail_items mi JOIN item_instance ii ON ii.guid = mi.item_guid "
                "WHERE mi.receiver IN ({}) AND ii.itemEntry IN ({})", guids, entries));
        }

        if (!offline.empty() && !spells.empty())
        {
            if (QueryResult result = CharacterDatabase.Query("SELECT guid, spell FROM character_spell WHERE guid IN ({}) AND spell IN ({})",
                IdList(offline), IdList(spells)))
            {
                do
                {
                    Field* fields = result->Fetch();
                    knownSpells.insert(Key(fields[1].Get<uint32>(), fields[0].Get<uint32>()));
                } while (result->NextRow());
            }
        }

        for (Offer const* offer : wanted)
        {
            bool anyKnows = false;
            bool anyHas = false;
            for (Character const& character : characters)
            {
                bool has = false;
                bool knows = false;

                if (offer->service)
                    has = character.online ? character.online->HasAtLoginFlag(ServiceFlag(offer->service))
                                           : (character.atLogin & ServiceFlag(offer->service)) != 0;
                else if (Player* player = character.online)
                {
                    knows = offer->teaches && player->HasSpell(offer->teaches);
                    has = knows || player->GetItemCount(offer->item, true) > 0;
                    for (auto const& [itemGuid, mailed] : player->mMitems)
                        has |= mailed && mailed->GetEntry() == offer->item;
                }
                else
                {
                    knows = offer->teaches && knownSpells.count(Key(offer->teaches, character.guid));
                    has = knows || heldItems.count(Key(offer->item, character.guid));
                }

                anyKnows |= knows;
                has |= ordersInFlight.count(Key(offer->id, character.guid)) > 0;
                anyHas |= has;
                if (has)
                    owned.insert(Key(offer->id, character.guid));
            }

            // With account-wide mounts and companions, one character knowing it is all of them
            // knowing it, at their next login. An offer bought for the account is the account's
            // from the order on (in flight, in the sharing module's table, or known to one).
            bool accountHas = AccountWide(*offer) && (anyHas || accountSpells.count(offer->teaches));
            if ((anyKnows && shopConfig.accountWideSpells) || accountHas)
                for (Character const& character : characters)
                    owned.insert(Key(offer->id, character.guid));
        }

        return owned;
    }

    void Answer(ChatHandler* handler, std::string const& json)
    {
        handler->SendSysMessage(json);
    }

    bool Refuse(ChatHandler* handler, std::string_view error, std::string_view message)
    {
        Answer(handler, fmt::format("{{\"ok\":false,\"error\":{},\"message\":{}}}", JsonString(error), JsonString(message)));
        return true;    // an answer, not a failed command: SOAP passes it on as it is
    }

    void Deliver(Offer const& offer, uint32 account, std::vector<Character> const& characters, Character const& character,
        CharacterDatabaseTransaction trans)
    {
        if (AccountWide(offer))
        {
            // Into the sharing module's table, which every character reads at its login; the ones
            // in the game learn it now (their learning writes the same row again, harmlessly).
            trans->Append("INSERT IGNORE INTO {} (account_id, spell) VALUES ({}, {})", ACCOUNT_TABLES[offer.accountKind], account, offer.teaches);
            for (Character const& online : characters)
                if (online.online && !online.online->HasSpell(offer.teaches))
                    online.online->learnSpell(offer.teaches);
            return;
        }

        if (offer.item)
        {
            MailDraft draft(fmt::format("Your order: {}", offer.name),
                fmt::format("Here's the {} you bought, {}. Pleasure doing business. Take it out of the mailbox within {} days, "
                    "or it's gone.\n\n-- Pix Xizzix, Undermine Real Exchange", offer.name, character.name, SHOP_MAIL_DAYS));
            if (Item* item = Item::CreateItem(offer.item, offer.count, character.online))
            {
                item->SaveToDB(trans);
                draft.AddItem(item);
            }
            // Kept as long as the mail lasts: a creature can't take back a mail that expires, so
            // the item would be gone.
            draft.SendMailTo(trans, MailReceiver(character.online, character.guid), MailSender(MAIL_CREATURE, NPC_PIX_XIZZIX),
                MAIL_CHECK_MASK_NONE, 0, SHOP_MAIL_DAYS);
            return;
        }

        AtLoginFlags flag = ServiceFlag(offer.service);
        // Written now for a character in the game too, so a crash before its next save can't
        // lose what was paid for. The save that follows writes the same flag.
        trans->Append("UPDATE characters SET at_login = at_login | {} WHERE guid = {}", uint32(flag), character.guid);
        if (character.online)
            character.online->SetAtLoginFlag(flag);
    }

    void TellPlayer(Offer const& offer, Character const& character)
    {
        Player* player = character.online;
        if (!player || !player->GetSession())
            return;

        ChatHandler chat(player->GetSession());
        if (AccountWide(offer))
            chat.PSendSysMessage("You bought the {} in the shop. It's yours now, on every character of your account.", offer.name);
        else if (offer.item)
            chat.PSendSysMessage("Pix Xizzix has mailed you the {} you bought in the shop. Check your mailbox.", offer.name);
        else
            chat.PSendSysMessage("You bought a {} in the shop. Log out to the character list to use it.", offer.name);
    }

    // An account-wide purchase is told to every real character of the account in the game, not
    // only the one it was bought "for".
    void TellAccount(Offer const& offer, std::vector<Character> const& characters, Character const& character)
    {
        if (!AccountWide(offer))
            return TellPlayer(offer, character);

        for (Character const& online : characters)
            if (online.online && IsRealPlayer(online.online))
                TellPlayer(offer, online);
    }
}

std::string BossCoins::JsonString(std::string_view text)
{
    std::string out = "\"";
    for (char c : text)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += fmt::format("\\u{:04x}", c);
                else
                    out += c;
        }
    }
    return out + "\"";
}

void BossCoins::Shop::Load()
{
    offers.clear();

    uint32 skipped = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT id, category, name, description, icon, price, item, count, service, phase "
        "FROM mod_boss_coins_shop WHERE enabled = 1 ORDER BY sort, id"))
    {
        do
        {
            Field* fields = result->Fetch();
            Offer offer;
            offer.id = fields[0].Get<uint32>();
            offer.category = fields[1].Get<std::string>();
            offer.name = fields[2].Get<std::string>();
            offer.description = fields[3].Get<std::string>();
            offer.icon = fields[4].Get<std::string>();
            offer.price = fields[5].Get<uint32>();
            offer.item = fields[6].Get<uint32>();
            offer.count = std::max<uint32>(fields[7].Get<uint8>(), 1);
            offer.service = fields[8].Get<uint8>();
            offer.phase = std::min<uint8>(fields[9].Get<uint8>(), PROGRESSION_LAST_PHASE);

            if ((offer.item != 0) == (offer.service != SERVICE_NONE)
                || (offer.service && ServiceFlag(offer.service) == AT_LOGIN_NONE))
            {
                LOG_ERROR("server.loading", "mod-boss-coins: shop offer {} needs either an item or a service (1-4); skipped", offer.id);
                ++skipped;
                continue;
            }

            if (offer.item)
            {
                // Rows for another module's items on a server without that module.
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.item);
                if (!proto)
                {
                    ++skipped;
                    continue;
                }

                offer.count = std::min<uint32>(offer.count, proto->GetMaxStackSize());
                offer.requiredLevel = proto->RequiredLevel;
                offer.requiredSkill = proto->RequiredSkill;
                offer.requiredSkillRank = proto->RequiredSkillRank;
                if (offer.name.empty())
                    offer.name = proto->Name1;
                for (auto const& spell : proto->Spells)
                    if (spell.SpellTrigger == ITEM_SPELLTRIGGER_LEARN_SPELL_ID && spell.SpellId > 0)
                        offer.teaches = uint32(spell.SpellId);
                if (offer.teaches)
                    offer.accountKind = AccountKindOf(offer.teaches);
            }
            else if (offer.name.empty())
                offer.name = ServiceName(offer.service);

            offers.push_back(std::move(offer));
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "mod-boss-coins: {} shop offer(s) loaded, {} skipped", offers.size(), skipped);

    // The sharing modules' tables, for account-wide purchases. Looked for even with the setting
    // off: .reload config can turn it on without a .coinshop reload.
    for (uint8 kind = ACCOUNT_MOUNTS; kind <= ACCOUNT_PETS; ++kind)
    {
        accountTables[kind] = CharacterDatabase.Query("SHOW TABLES LIKE '{}'", ACCOUNT_TABLES[kind]) != nullptr;
        if (shopConfig.accountWideSpells && !accountTables[kind])
            LOG_INFO("server.loading", "mod-boss-coins: BossCoins.Shop.AccountWideSpells is on but the characters database has no {} table; "
                "the shop mails {} to the character instead", ACCOUNT_TABLES[kind], kind == ACCOUNT_MOUNTS ? "mounts" : "companions");
    }

    progressionQuests = sObjectMgr->GetQuestTemplate(PROGRESSION_QUEST + 1) != nullptr;
    if (!progressionQuests && std::any_of(offers.begin(), offers.end(), [](Offer const& offer) { return offer.phase; }))
        LOG_INFO("server.loading", "mod-boss-coins: no mod-individual-progression quests on this server; the shop sells every offer to everyone");
}

class BossCoinsShopCommandScript : public CommandScript
{
public:
    BossCoinsShopCommandScript() : CommandScript("BossCoinsShopCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        // SEC_CONSOLE: no account in the game is that high, so only the console and SOAP run them.
        static ChatCommandTable shopCommandTable =
        {
            { "list",   HandleListCommand,   SEC_CONSOLE,       Console::Yes },
            { "buy",    HandleBuyCommand,    SEC_CONSOLE,       Console::Yes },
            { "reload", HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "coinshop", shopCommandTable },
        };

        return commandTable;
    }

    static bool HandleListCommand(ChatHandler* handler, uint32 account)
    {
        if (!shopConfig.enabled)
            return Refuse(handler, "closed", "The shop is closed.");

        std::vector<Character> characters = LoadCharacters(account);
        std::vector<Offer const*> all;
        for (Offer const& offer : offers)
            all.push_back(&offer);
        LoadSkills(characters, all);
        std::unordered_set<uint64> owned = Owned(account, characters, all);
        uint8 phase = AccountPhase(characters);

        std::string json = fmt::format("{{\"ok\":true,\"balance\":{},\"phase\":{},\"characters\":[", Wallet::Balance(account), phase);
        for (size_t i = 0; i < characters.size(); ++i)
        {
            Character const& c = characters[i];
            json += fmt::format("{}{{\"guid\":{},\"name\":{},\"race\":{},\"class\":{},\"gender\":{},\"level\":{},\"online\":{}}}",
                i ? "," : "", c.guid, JsonString(c.name), c.race, c.cls, c.gender, c.level, c.online ? "true" : "false");
        }

        json += "],\"offers\":[";
        for (size_t i = 0; i < offers.size(); ++i)
        {
            Offer const& o = offers[i];
            std::vector<uint32> have, cannot;
            for (Character const& c : characters)
            {
                if (owned.count(Key(o.id, c.guid)))
                    have.push_back(c.guid);
                if (!CannotUse(o, c).empty())
                    cannot.push_back(c.guid);
            }

            bool locked = Locked(o, phase);
            json += fmt::format("{}{{\"id\":{},\"category\":{},\"name\":{},\"description\":{},\"icon\":{},\"price\":{},"
                "\"phase\":{},\"locked\":{},\"unlock\":{},\"account\":{},\"owned\":[{}],\"cannot\":[{}]}}",
                i ? "," : "", o.id, JsonString(o.category), JsonString(o.name), JsonString(o.description), JsonString(o.icon),
                o.price, o.phase, locked ? "true" : "false", JsonString(locked ? UnlockText(o) : ""), AccountWide(o) ? "true" : "false",
                IdList(have), IdList(cannot));
        }

        Answer(handler, json + "]}");
        return true;
    }

    static bool HandleBuyCommand(ChatHandler* handler, uint32 account, uint32 offerId, uint32 guid)
    {
        if (!shopConfig.enabled)
            return Refuse(handler, "closed", "The shop is closed.");

        // A character on its way into the world has read its row already, and not yet joined
        // the players that are asked directly: an order now would be lost on its first save.
        if (WorldSession* session = sWorldSessionMgr->FindSession(account))
            if (session->PlayerLoading())
                return Refuse(handler, "busy", "A character of yours is logging in. Try again in a moment.");

        Offer const* offer = FindOffer(offerId);
        if (!offer)
            return Refuse(handler, "no_offer", "That isn't for sale any more.");

        std::vector<Character> characters = LoadCharacters(account);
        auto itr = std::find_if(characters.begin(), characters.end(), [guid](Character const& c) { return c.guid == guid; });
        if (itr == characters.end())
            return Refuse(handler, "no_character", "That character isn't on your account.");

        if (Locked(*offer, AccountPhase(characters)))
            return Refuse(handler, "locked", PastLimit(*offer)
                ? fmt::format("The {} isn't for sale yet: it comes to the realm with {}.", offer->name, Expansion(offer->phase))
                : fmt::format("The {} isn't for sale to you yet. {} with any character to unlock it.", offer->name, PhaseUnlock(offer->phase)));

        LoadSkills(characters, { offer });
        Character const& character = *itr;
        if (std::string why = CannotUse(*offer, character); !why.empty())
            return Refuse(handler, "cannot", why);

        bool accountWide = AccountWide(*offer);
        if (Owned(account, characters, { offer }).count(Key(offer->id, character.guid)))
            return Refuse(handler, "owned", offer->service
                ? fmt::format("{} has a {} waiting already: log in to the character list to use it.", character.name, offer->name)
                : accountWide
                ? fmt::format("Your account has the {} already: every character of yours can use it.", offer->name)
                : fmt::format("{} has the {} already. Look in the bags, the bank and the mailbox.", character.name, offer->name));

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!Wallet::Take(account, character.guid, offer->price, "shop", offer->id, trans))
            return Refuse(handler, "balance", fmt::format("The {} costs {} coins, and your wallet holds {}.",
                offer->name, offer->price, Wallet::Balance(account)));

        Deliver(*offer, account, characters, character, trans);

        uint64 key = Key(offer->id, character.guid);
        ++ordersInFlight[key];

        bool watchLogin = offer->service && !character.online;
        if (watchLogin)
        {
            std::lock_guard<std::mutex> guard(serviceOrdersLock);
            ServiceOrder& order = serviceOrders[character.guid];
            order.flags |= ServiceFlag(offer->service);
            ++order.inFlight;
        }

        orderCommits.AddCallback(CharacterDatabase.AsyncCommitTransaction(trans)).AfterComplete(
            [key, account, watchLogin, offerId = offer->id, guid = character.guid, price = offer->price](bool success)
            {
                auto itr = ordersInFlight.find(key);
                if (itr != ordersInFlight.end() && !--itr->second)
                    ordersInFlight.erase(itr);

                if (watchLogin)
                {
                    std::lock_guard<std::mutex> guard(serviceOrdersLock);
                    if (auto order = serviceOrders.find(guid); order != serviceOrders.end() && order->second.inFlight)
                        --order->second.inFlight;
                }

                // The coins went out of the wallet in the same transaction, so the database still
                // has them; only the balance in memory is short until the next restart.
                if (!success)
                    LOG_ERROR("module", "mod-boss-coins: the order of shop offer {} for character {} (account {}, {} coins) "
                        "couldn't be written; nothing was delivered. The wallet shows {} coins less until the next restart.",
                        offerId, guid, account, price, price);
            });

        TellAccount(*offer, characters, character);

        LOG_INFO("module", "mod-boss-coins: account {} bought shop offer {} ({}) for {} ({}){}, {} coins",
            account, offer->id, offer->name, character.name, character.guid, accountWide ? " and the whole account" : "", offer->price);

        std::string message = accountWide
            ? (character.online
                ? fmt::format("The {} is yours: {} knows it now, and your other characters learn it when they log in.", offer->name, character.name)
                : fmt::format("The {} is yours: every character of your account learns it when it logs in.", offer->name))
            : offer->item
            ? fmt::format("The {} is in {}'s mailbox.", offer->name, character.name)
            : fmt::format("{} gets the {} at the character list.", character.name, offer->name);
        Answer(handler, fmt::format("{{\"ok\":true,\"balance\":{},\"message\":{}}}", Wallet::Balance(account), JsonString(message)));
        return true;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        Shop::Load();
        handler->PSendSysMessage("mod-boss-coins: {} shop offer(s) loaded.", offers.size());
        return true;
    }
};

class BossCoinsShopWorldScript : public WorldScript
{
public:
    BossCoinsShopWorldScript() : WorldScript("BossCoinsShopWorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 /*diff*/) override
    {
        orderCommits.ProcessReadyCallbacks();
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        shopConfig.enabled = sConfigMgr->GetOption<bool>("BossCoins.Shop.Enable", true);
        shopConfig.accountWideSpells = sConfigMgr->GetOption<bool>("BossCoins.Shop.AccountWideSpells", false);
        shopConfig.progressionGate = sConfigMgr->GetOption<bool>("BossCoins.Shop.ProgressionGate", true);
        // mod-individual-progression's setting (one config for every module); absent without it.
        shopConfig.progressionLimit = sConfigMgr->GetOption<uint32>("IndividualProgression.ProgressionLimit", 0, false);
    }
};

class BossCoinsShopPlayerScript : public PlayerScript
{
public:
    BossCoinsShopPlayerScript() : PlayerScript("BossCoinsShopPlayerScript", { PLAYERHOOK_ON_LOGIN }) { }

    // A service ordered while the character was offline: see serviceOrders.
    void OnPlayerLogin(Player* player) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        ServiceOrder order;
        {
            std::lock_guard<std::mutex> guard(serviceOrdersLock);
            auto itr = serviceOrders.find(guid);
            if (itr == serviceOrders.end())
                return;

            order = itr->second;
            serviceOrders.erase(itr);
        }

        uint32 missing = 0;
        for (uint8 service = SERVICE_RENAME; service <= SERVICE_CHANGE_FACTION; ++service)
        {
            AtLoginFlags flag = ServiceFlag(service);
            if ((order.flags & flag) && !player->HasAtLoginFlag(flag))
                missing |= flag;
        }

        if (!missing)
            return;

        // Written already: only what the row still has. Not yet: all of it, the write brings it.
        if (!order.inFlight)
        {
            QueryResult result = CharacterDatabase.Query("SELECT at_login FROM characters WHERE guid = {}", guid);
            missing &= result ? result->Fetch()[0].Get<uint16>() : 0;
        }

        for (uint8 service = SERVICE_RENAME; service <= SERVICE_CHANGE_FACTION; ++service)
            if (missing & ServiceFlag(service))
                player->SetAtLoginFlag(ServiceFlag(service));
    }
};

void AddBossCoinsShopScripts()
{
    new BossCoinsShopCommandScript();
    new BossCoinsShopPlayerScript();
    new BossCoinsShopWorldScript();
}
