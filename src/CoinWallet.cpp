/*
 * mod-boss-coins: the account wallet.
 *
 * Coins that belong to an account instead of a character's bags. The launcher's shop spends
 * from it (CoinShop.cpp), the boss coins and the daily login coins go into it, and Pix Xizzix
 * moves coins between a character's bags and the wallet: "put my coins in my wallet" takes every
 * coin the character carries (bags and bank), "take coins out" offers a few amounts. Her own
 * stock costs bag coins, and when the bags are short she takes the rest out of the wallet
 * (BossCoins.cpp). The wallet has no cap; a character's bags still hold at most
 * the coin's own maximum.
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
#include <limits>
#include <mutex>
#include <unordered_map>

using namespace BossCoins;
using namespace Acore::ChatCommands;

namespace
{
    // account -> coins. Boss coins come in from the map threads, several at once, and the rest
    // of the changes happen on the world thread: every change is checked and made under the
    // lock, in Change, so any thread may make one.
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

    // Coins in (`change` above 0), stopping at the most a wallet holds, or out (below 0), all of
    // them or none when the wallet has fewer. Returns the change made, 0 for none; `balanceAfter`
    // gets the balance it left. `trans` is the caller's, to commit with the bags the coins came
    // out of or went into; with none the change is committed on its own.
    int64 Change(uint32 account, uint32 guid, int64 change, std::string_view reason, uint32 offer,
        CharacterDatabaseTransaction trans, uint32* balanceAfter = nullptr)
    {
        uint32 balance;
        {
            std::lock_guard<std::mutex> guard(walletLock);
            uint32& coins = balances[account];
            if (change > 0)
                change = std::min<int64>(change, int64(Wallet::MAX_BALANCE) - coins);
            else if (-change > int64(coins))
                change = 0;

            coins = uint32(int64(coins) + change);
            balance = coins;
        }

        if (balanceAfter)
            *balanceAfter = balance;
        if (!change)
            return 0;

        bool own = !trans;
        if (own)
            trans = CharacterDatabase.BeginTransaction();

        Record(trans, account, guid, change, balance, reason, offer);

        if (own)
            CharacterDatabase.CommitTransaction(trans);
        return change;
    }
}

uint32 BossCoins::Wallet::Balance(uint32 account)
{
    std::lock_guard<std::mutex> guard(walletLock);
    auto itr = balances.find(account);
    return itr != balances.end() ? itr->second : 0;
}

uint32 BossCoins::Wallet::Add(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer,
    CharacterDatabaseTransaction trans, uint32* balance)
{
    return uint32(Change(account, guid, int64(coins), reason, offer, trans, balance));
}

bool BossCoins::Wallet::Take(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer,
    CharacterDatabaseTransaction trans)
{
    return !coins || Change(account, guid, -int64(coins), reason, offer, trans);
}

uint32 BossCoins::Wallet::ToBags(Player* player, uint32 coins, std::string_view reason, bool all)
{
    uint32 account = player->GetSession()->GetAccountId();
    uint32 guid = player->GetGUID().GetCounter();

    uint32 count = std::min({ coins, Balance(account), CoinRoom(player) });
    if (!count || (all && count < coins))
        return 0;

    // As many as the bags have room for.
    uint32 noSpace = 0;
    ItemPosCountVec dest;
    if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_TARNISHED_UNDERMINE_REAL, count, &noSpace) != EQUIP_ERR_OK)
    {
        count -= std::min(count, noSpace);
        dest.clear();
        if (!count || (all && count < coins) ||
            player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, ITEM_TARNISHED_UNDERMINE_REAL, count) != EQUIP_ERR_OK)
            return 0;
    }

    // Out of the wallet first: no coin reaches the bags that the wallet didn't give up.
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    if (!Take(account, guid, count, reason, 0, trans))
        return 0;

    Item* item = player->StoreNewItem(dest, ITEM_TARNISHED_UNDERMINE_REAL, true);
    if (!item)
    {
        Add(account, guid, count, reason, 0, trans);
        CharacterDatabase.CommitTransaction(trans);
        return 0;
    }

    player->SendNewItem(item, count, true, false);
    player->SaveInventoryAndGoldToDB(trans);
    CharacterDatabase.CommitTransaction(trans);
    return count;
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
        ACTION_DEPOSIT       = 1,
        ACTION_WITHDRAW_MENU = 2,
        ACTION_WITHDRAW_ALL  = 3,
        // + the number of coins
        ACTION_WITHDRAW      = 1000,
    };

    // The amounts her "take coins out" menu offers, besides "as many as I can carry". A 3.3.5
    // client asks for a typed number only as an "Enter Code" box that shows none of the server's
    // text, so no number is typed.
    constexpr uint32 WITHDRAW_AMOUNTS[] = { 10, 25, 50, 100 };

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

        // Into the wallet first, as many as it has room for (boss coins can come in on another
        // thread at any moment), and only those out of the bags: no coin leaves the bags that the
        // wallet didn't take.
        uint32 guid = player->GetGUID().GetCounter();
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        uint32 added = Wallet::Add(account, guid, before, "deposit", 0, trans);
        if (!added)
        {
            chat.SendSysMessage("Your account wallet is full.");
            return;
        }

        player->DestroyItemCount(ITEM_TARNISHED_UNDERMINE_REAL, added, true);
        uint32 after = player->GetItemCount(ITEM_TARNISHED_UNDERMINE_REAL, true);
        uint32 moved = before > after ? before - after : 0;

        // Fewer left the bags than the wallet took: it gives the rest back.
        if (moved < added)
            Wallet::Take(account, guid, added - moved, "deposit", 0, trans);

        if (moved)
            player->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
        if (!moved)
            return;

        chat.PSendSysMessage("You put {} {} in your account wallet. It now holds {}.", moved, CoinName(moved),
            Wallet::Balance(account));
        if (added < before)
            chat.SendSysMessage("Your account wallet is full; the rest stay with you.");
    }

    void Withdraw(Player* player, uint32 wanted)
    {
        ChatHandler chat(player->GetSession());
        uint32 account = player->GetSession()->GetAccountId();
        uint32 balance = Wallet::Balance(account);

        uint32 room = CoinRoom(player);
        if (!std::min({ wanted, balance, room }))
        {
            if (!balance)
                chat.SendSysMessage("Your account wallet is empty.");
            else
                chat.SendSysMessage("This character can't carry any more Tarnished Undermine Reals.");
            return;
        }

        uint32 count = Wallet::ToBags(player, wanted, "withdraw");
        if (!count)
        {
            player->SendEquipError(EQUIP_ERR_INVENTORY_FULL, nullptr, nullptr, ITEM_TARNISHED_UNDERMINE_REAL);
            return;
        }

        chat.PSendSysMessage("You take {} {} out of your account wallet. It holds {} now.", count, CoinName(count),
            Wallet::Balance(account));

        // Fewer than asked for: say what stopped the rest.
        if (count < wanted)
        {
            if (count == balance)
                chat.SendSysMessage("That was all your wallet had.");
            else if (count == room)
                chat.PSendSysMessage("A character can't carry more than {} Tarnished Undermine Reals; the rest stay in the wallet.",
                    sObjectMgr->GetItemTemplate(ITEM_TARNISHED_UNDERMINE_REAL)->MaxCount);
            else
                chat.SendSysMessage("Your bags had no room for more; the rest stay in the wallet.");
        }
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
                SENDER_WALLET, ACTION_WITHDRAW_MENU);

        player->SendPreparedGossip(creature);
        return true;
    }

    void ShowWithdrawMenu(Player* player, Creature* creature)
    {
        uint32 balance = Wallet::Balance(player->GetSession()->GetAccountId());
        if (!balance)
        {
            OnGossipHello(player, creature);
            return;
        }

        ClearGossipMenuFor(player);
        for (uint32 amount : WITHDRAW_AMOUNTS)
            if (amount < balance)
                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, fmt::format("Take out {}.", amount),
                    SENDER_WALLET, ACTION_WITHDRAW + amount);

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, fmt::format("Take out as many as I can carry (the wallet holds {}).",
            balance), SENDER_WALLET, ACTION_WITHDRAW_ALL);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back.", SENDER_WALLET, 0);
        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature);
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != SENDER_WALLET)
            return false;

        if (action == ACTION_WITHDRAW_MENU)
        {
            ShowWithdrawMenu(player, creature);
            return true;
        }

        if (action == ACTION_DEPOSIT)
            Deposit(player);
        else if (action == ACTION_WITHDRAW_ALL)
            Withdraw(player, std::numeric_limits<uint32>::max());
        else if (action > ACTION_WITHDRAW)
            Withdraw(player, action - ACTION_WITHDRAW);

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
            if (Wallet::Add(account.GetID(), 0, uint32(coins), "gm") < uint32(coins))
                handler->PSendSysMessage("A wallet holds at most {} coins.", Wallet::MAX_BALANCE);
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
