/*
 * mod-boss-coins: the account wallet.
 *
 * Coins that belong to an account instead of a character's bags. The launcher's shop spends
 * from it (CoinShop.cpp), the daily login coins go into it, and Pix Xizzix moves coins between
 * a character's bags and the wallet: "put my coins in my wallet" takes every coin the character
 * carries (bags and bank), "take coins out" asks how many. Bag coins are still what her own
 * stock costs. The wallet has no cap; a character's bags still hold at most the coin's own
 * maximum.
 *
 *   characters table mod_boss_coins_wallet   account -> coins, the balance
 *   characters table mod_boss_coins_ledger   every change, with why and from which character
 *
 * The balances are read at startup and kept in memory; every change is written at once, in the
 * same transaction as the bags it came from or went into, so a crash can't leave coins in both
 * places or in neither. The database rows change by the difference, not to the new balance, so
 * writes that a worker applies out of order still add up.
 *
 * .wallet                         your account's balance
 * .wallet adjust <account> <n>    add (or with a minus, take) coins; a game master's fix
 *
 * Released under the MIT License.
 */

#include "BossCoins.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "GossipDef.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <unordered_map>

using namespace BossCoins;
using namespace Acore::ChatCommands;

namespace
{
    // account -> coins. Changed on the world thread only; the lock is for the odd reader
    // elsewhere.
    std::mutex walletLock;
    std::unordered_map<uint32, uint32> balances;

    void Record(CharacterDatabaseTransaction trans, uint32 account, uint32 guid, int64 change, uint32 balance,
        std::string_view reason, uint32 offer)
    {
        trans->Append("INSERT INTO mod_boss_coins_wallet (account, coins) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE coins = coins + {}", account, change, change);
        trans->Append("INSERT INTO mod_boss_coins_ledger (at, account, guid, coins, balance, reason, offer) "
            "VALUES ({}, {}, {}, {}, {}, '{}', {})",
            GameTime::GetGameTime().count(), account, guid, change, balance, reason, offer);
    }

    // `trans` is the caller's, to commit with the bags the coins came out of or went into; with
    // none the change is committed on its own.
    void Change(uint32 account, uint32 guid, int64 change, std::string_view reason, uint32 offer,
        CharacterDatabaseTransaction trans)
    {
        uint32 balance;
        {
            std::lock_guard<std::mutex> guard(walletLock);
            uint32& coins = balances[account];
            coins = uint32(int64(coins) + change);
            balance = coins;
        }

        bool own = !trans;
        if (own)
            trans = CharacterDatabase.BeginTransaction();

        Record(trans, account, guid, change, balance, reason, offer);

        if (own)
            CharacterDatabase.CommitTransaction(trans);
    }
}

uint32 BossCoins::Wallet::Balance(uint32 account)
{
    std::lock_guard<std::mutex> guard(walletLock);
    auto itr = balances.find(account);
    return itr != balances.end() ? itr->second : 0;
}

void BossCoins::Wallet::Add(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer,
    CharacterDatabaseTransaction trans)
{
    coins = std::min(coins, MAX_BALANCE - Balance(account));
    if (coins)
        Change(account, guid, int64(coins), reason, offer, trans);
}

bool BossCoins::Wallet::Take(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer,
    CharacterDatabaseTransaction trans)
{
    if (Balance(account) < coins)
        return false;

    if (coins)
        Change(account, guid, -int64(coins), reason, offer, trans);
    return true;
}

void BossCoins::Wallet::Load()
{
    std::lock_guard<std::mutex> guard(walletLock);
    balances.clear();

    // A balance below 0 can only be the leftover of a change that failed. The server counts it as
    // 0, so the row has to say 0 too, or every change after it would be off by as much.
    CharacterDatabase.DirectExecute("UPDATE mod_boss_coins_wallet SET coins = 0 WHERE coins < 0");

    if (QueryResult result = CharacterDatabase.Query("SELECT account, coins FROM mod_boss_coins_wallet WHERE coins > 0"))
    {
        do
        {
            Field* fields = result->Fetch();
            balances[fields[0].Get<uint32>()] = uint32(fields[1].Get<int32>());
        } while (result->NextRow());
    }

    LOG_INFO("server.loading", "mod-boss-coins: {} account wallet(s) with coins in them", balances.size());
}

namespace
{
    enum PixWalletGossip : uint32
    {
        // Pix's own options (her gossip_menu_option rows) come with sender 0.
        SENDER_WALLET    = NPC_PIX_XIZZIX,
        ACTION_DEPOSIT   = 1,
        ACTION_WITHDRAW  = 2,
    };

    void Deposit(Player* player)
    {
        ChatHandler chat(player->GetSession());
        uint32 account = player->GetSession()->GetAccountId();

        uint32 before = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
        if (!before)
        {
            chat.SendSysMessage("You aren't carrying any Tarnished Undermine Reals.");
            return;
        }

        if (before > Wallet::MAX_BALANCE - Wallet::Balance(account))
        {
            chat.SendSysMessage("Your account wallet is full.");
            return;
        }

        player->DestroyItemCount(ITEM_TARNISHED_UNDERMINE_REAL, before, true);
        uint32 after = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
        uint32 moved = before > after ? before - after : 0;
        if (!moved)
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        Wallet::Add(account, player->GetGUID().GetCounter(), moved, "deposit", 0, trans);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        chat.PSendSysMessage("You put {} {} in your account wallet. It now holds {}.", moved, CoinName(moved),
            Wallet::Balance(account));
    }

    void Withdraw(Player* player, uint32 wanted)
    {
        ChatHandler chat(player->GetSession());
        uint32 account = player->GetSession()->GetAccountId();
        uint32 balance = Wallet::Balance(account);

        uint32 count = std::min({ wanted, balance, CoinRoom(player) });
        if (!count)
        {
            if (!wanted)
                chat.SendSysMessage("Type how many coins you want to take out, as a number.");
            else if (!balance)
                chat.SendSysMessage("Your account wallet is empty.");
            else
                chat.SendSysMessage("This character can't carry any more Tarnished Undermine Reals.");
            return;
        }

        // As many as the bags have room for.
        uint32 noSpace = 0;
        ItemPosCountVec dest;
        InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_TARNISHED_UNDERMINE_REAL, count, &noSpace);
        if (result != EQUIP_ERR_OK)
        {
            count -= std::min(count, noSpace);
            dest.clear();
            if (!count || player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_TARNISHED_UNDERMINE_REAL, count) != EQUIP_ERR_OK)
            {
                player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr, ITEM_TARNISHED_UNDERMINE_REAL);
                return;
            }
        }

        // Out of the wallet first: no coin reaches the bags that the wallet didn't give up.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!Wallet::Take(account, player->GetGUID().GetCounter(), count, "withdraw", 0, trans))
            return;

        Item* item = player->StoreNewItem(dest, ITEM_TARNISHED_UNDERMINE_REAL, true);
        if (!item)
        {
            Wallet::Add(account, player->GetGUID().GetCounter(), count, "withdraw", 0, trans);
            CharacterDatabase.CommitTransaction(trans);
            return;
        }

        player->SendNewItem(item, count, true, false);
        player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        chat.PSendSysMessage("You take {} {} out of your account wallet. It holds {} now.", count, CoinName(count),
            Wallet::Balance(account));
    }

    uint32 ParseCount(char const* code)
    {
        std::string_view text = code ? code : "";
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);

        uint32 value = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc() ? value : 0;
    }
}

// Pix Xizzix: her own menu (the pages of her stock, from the database) plus the wallet's two
// options. The database options keep their own handling: the core's, when this script answers
// that a selection isn't one of its own.
class npc_boss_coins_pix : public CreatureScript
{
public:
    npc_boss_coins_pix() : CreatureScript("npc_boss_coins_pix") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        uint32 balance = Wallet::Balance(player->GetSession()->GetAccountId());
        uint32 carried = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);

        if (carried)
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, fmt::format("Put the {} {} I carry in my account wallet.",
                carried, CoinName(carried)), SENDER_WALLET, ACTION_DEPOSIT);

        if (balance)
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, fmt::format("Take coins out of my account wallet ({} in it).", balance),
                SENDER_WALLET, ACTION_WITHDRAW, fmt::format("How many? Your account wallet holds {}.", balance), 0, true);

        player->SendPreparedGossip(creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != SENDER_WALLET)
            return false;

        if (action == ACTION_DEPOSIT)
            Deposit(player);

        return OnGossipHello(player, creature);
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, char const* code) override
    {
        if (sender != SENDER_WALLET)
            return false;

        if (action == ACTION_WITHDRAW)
            Withdraw(player, ParseCount(code));

        return OnGossipHello(player, creature);
    }
};

class BossCoinsWalletCommandScript : public CommandScript
{
public:
    BossCoinsWalletCommandScript() : CommandScript("BossCoinsWalletCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable walletCommandTable =
        {
            { "",       HandleWalletCommand,       SEC_PLAYER,        Console::No  },
            { "adjust", HandleWalletAdjustCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "wallet", walletCommandTable },
        };

        return commandTable;
    }

    static bool HandleWalletCommand(ChatHandler* handler)
    {
        uint32 balance = Wallet::Balance(handler->GetSession()->GetAccountId());
        handler->PSendSysMessage("Your account wallet holds {} {}. Spend them in the launcher's shop, or take some out at "
            "Pix Xizzix in Booty Bay.", balance, CoinName(balance));
        return true;
    }

    static bool HandleWalletAdjustCommand(ChatHandler* handler, AccountIdentifier account, int32 coins)
    {
        if (!coins)
            return false;

        if (coins > 0)
        {
            if (uint32(coins) > Wallet::MAX_BALANCE - Wallet::Balance(account.GetID()))
            {
                handler->SendErrorMessage("A wallet holds at most {} coins.", Wallet::MAX_BALANCE);
                return false;
            }
            Wallet::Add(account.GetID(), 0, uint32(coins), "gm");
        }
        else if (!Wallet::Take(account.GetID(), 0, uint32(-int64(coins)), "gm"))
        {
            handler->SendErrorMessage("{}'s wallet holds only {}.", account.GetName(), Wallet::Balance(account.GetID()));
            return false;
        }

        handler->PSendSysMessage("{}'s account wallet holds {} now.", account.GetName(), Wallet::Balance(account.GetID()));
        return true;
    }
};

void AddBossCoinsWalletScripts()
{
    new npc_boss_coins_pix();
    new BossCoinsWalletCommandScript();
}
