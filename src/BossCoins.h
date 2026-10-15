/*
 * mod-boss-coins: what the module's files share.
 *
 * Released under the MIT License.
 */

#ifndef MOD_BOSS_COINS_H
#define MOD_BOSS_COINS_H

#include "DatabaseEnvFwd.h"
#include "Define.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

class Player;

namespace BossCoins
{
    constexpr uint32 ITEM_TARNISHED_UNDERMINE_REAL = 90410;
    // Pix Xizzix, and the creature entries that own the pages of her stock (the module's SQL).
    constexpr uint32 NPC_PIX_XIZZIX = 9503200;
    constexpr uint32 NPC_PIX_XIZZIX_LAST_PAGE = 9503299;

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

    // A player with a client: not a bot.
    bool IsRealPlayer(Player* player);

    char const* CoinName(uint32 count);

    // How many more coins the character can own, counting the bank and what is waiting in the
    // mail.
    uint32 CoinRoom(Player* player);

    // The account wallet (CoinWallet.cpp): coins that belong to an account rather than to a
    // character's bags, spent in the launcher's shop. Any thread may change a balance (boss kills
    // pay on the map threads): each change is checked and made under one lock.
    namespace Wallet
    {
        uint32 Balance(uint32 account);

        // The most a wallet holds (its column is a signed INT). Add stops there.
        constexpr uint32 MAX_BALANCE = 2147483647;

        // `guid` is the character the change came from or went to, 0 for none. `reason` is a
        // short word for the ledger: boss, daily, deposit, withdraw, shop, gm. `trans`: a
        // transaction of the caller's to write the change in, with what the coins paid for or
        // came from; none, and the change is written on its own. Returns the coins added, fewer
        // than asked when the wallet is full; `balance` gets the balance after.
        uint32 Add(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer = 0,
            CharacterDatabaseTransaction trans = nullptr, uint32* balance = nullptr);

        // False, and nothing taken, when the account has fewer coins than that.
        bool Take(uint32 account, uint32 guid, uint32 coins, std::string_view reason, uint32 offer = 0,
            CharacterDatabaseTransaction trans = nullptr);

        void Load();
    }

    // The launcher's shop (CoinShop.cpp).
    namespace Shop
    {
        void Load();
    }

    std::string JsonString(std::string_view text);
}

void AddBossCoinsWalletScripts();
void AddBossCoinsShopScripts();

#endif
