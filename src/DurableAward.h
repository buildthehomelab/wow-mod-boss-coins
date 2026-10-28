#ifndef BOSS_COINS_DURABLE_AWARD_H
#define BOSS_COINS_DURABLE_AWARD_H

#include <cstdint>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace BossCoins::Wallet
{
    enum class AwardStatus { Paid, AlreadyPaid, Deferred, Rejected, Uncertain };
    enum class ReceiptState { Missing, Found, Unavailable };
    struct DelveAward
    {
        std::uint64_t run;
        std::uint32_t account;
        std::uint32_t guid;
        std::uint32_t coins;
        bool operator==(DelveAward const& other) const
        {
            return run == other.run && account == other.account && guid == other.guid && coins == other.coins;
        }
        std::string Key() const { return "delve:" + std::to_string(run) + ":" + std::to_string(guid); }
    };
    struct AwardReceipt
    {
        ReceiptState state;
        DelveAward award;
    };

    // Called under the wallet lock. The cache already includes any legacy queued
    // deltas; a new durable credit adds its delta exactly once after confirmation.
    template<class ReadReceipt, class Commit>
    AwardStatus CreditDelveOnce(DelveAward const& award, std::uint32_t& balance, bool& blocked,
        ReadReceipt&& read, Commit&& commit)
    {
        constexpr std::uint32_t limit = 2147483647;
        if (blocked)
            return AwardStatus::Uncertain;
        if (!award.run || !award.account || !award.guid || !award.coins || award.coins > limit || balance > limit)
            return AwardStatus::Rejected;
        auto before = read(award);
        if (before.state == ReceiptState::Unavailable)
            return AwardStatus::Deferred;
        if (before.state == ReceiptState::Found)
            return before.award == award ? AwardStatus::AlreadyPaid : AwardStatus::Rejected;
        if (award.coins > limit - balance)
            return AwardStatus::Deferred; // All or nothing; never consume a key for a partial payment.
        commit(award, balance + award.coins);
        auto after = read(award);
        if (after.state != ReceiptState::Found || !(after.award == award))
        {
            blocked = true; // A commit may have succeeded without its acknowledgement.
            return AwardStatus::Uncertain;
        }
        balance += award.coins;
        return AwardStatus::Paid;
    }

    // An award cannot freeze an account while a prior bag/shop transaction is
    // still held by its caller (including compensation) or a database worker.
    template<class Transaction>
    bool HasPendingWalletTransactions(std::vector<std::weak_ptr<Transaction>>& pending)
    {
        pending.erase(std::remove_if(pending.begin(), pending.end(), [](auto const& transaction)
        {
            return transaction.expired();
        }), pending.end());
        return !pending.empty();
    }

    inline std::string DelveAwardStatement(DelveAward const& award,
        std::uint32_t balance, std::uint64_t timestamp)
    {
        return "CALL mod_boss_coins_delve_award(" + std::to_string(award.run) + "," +
            std::to_string(award.account) + "," + std::to_string(award.guid) + "," +
            std::to_string(award.coins) + "," + std::to_string(balance) + "," + std::to_string(timestamp) + ")";
    }
}
#endif
