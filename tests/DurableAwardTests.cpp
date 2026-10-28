#include "DurableAward.h"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace BossCoins::Wallet;

namespace
{
    void Check(bool value, char const* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    void TestAwards()
    {
        std::map<std::string, DelveAward> receipts;
        unsigned writes = 0;
        bool readable = true;
        bool loseAcknowledgement = false;
        std::uint32_t cache = 10;
        std::uint32_t durable = 10;
        bool blocked = false;
        auto read = [&](DelveAward const& award)
        {
            if (!readable)
                return AwardReceipt{ReceiptState::Unavailable, {}};
            auto found = receipts.find(award.Key());
            return found == receipts.end() ? AwardReceipt{ReceiptState::Missing, {}} :
                AwardReceipt{ReceiptState::Found, found->second};
        };
        auto commit = [&](DelveAward const& award, std::uint32_t)
        {
            ++writes;
            receipts.emplace(award.Key(), award);
            durable += award.coins;
            if (loseAcknowledgement)
                readable = false;
        };
        DelveAward first{100, 7, 42, 3};
        Check(CreditDelveOnce(first, cache, blocked, read, commit) == AwardStatus::Paid && cache == 13, "payment failed");
        Check(CreditDelveOnce(first, cache, blocked, read, commit) == AwardStatus::AlreadyPaid && writes == 1,
            "duplicate paid twice");
        DelveAward changed{100, 7, 42, 4};
        Check(CreditDelveOnce(changed, cache, blocked, read, commit) == AwardStatus::Rejected, "key rebound to new amount");
        changed = {100, 8, 42, 3};
        Check(CreditDelveOnce(changed, cache, blocked, read, commit) == AwardStatus::Rejected, "key rebound to new account");
        loseAcknowledgement = true;
        DelveAward second{101, 7, 42, 3};
        Check(CreditDelveOnce(second, cache, blocked, read, commit) == AwardStatus::Uncertain && blocked && cache == 13 && durable == 16,
            "unknown acknowledgement published cache");
        Check(CreditDelveOnce(second, cache, blocked, read, commit) == AwardStatus::Uncertain && writes == 2,
            "blocked account wrote again");
        // Restart reloads the real committed wallet; retry observes its receipt.
        cache = durable;
        blocked = false;
        readable = true;
        loseAcknowledgement = false;
        Check(CreditDelveOnce(second, cache, blocked, read, commit) == AwardStatus::AlreadyPaid && cache == 16,
            "restart duplicated lost acknowledgement");
        readable = false;
        DelveAward third{102, 7, 42, 3};
        Check(CreditDelveOnce(third, cache, blocked, read, commit) == AwardStatus::Deferred && !blocked && writes == 2,
            "unreadable initial state attempted payment");
        readable = true;
        Check(CreditDelveOnce(third, cache, blocked, read, commit) == AwardStatus::Paid && cache == 19,
            "new run could not farm another award");
        std::vector<std::weak_ptr<int>> pending;
        auto caller = std::make_shared<int>(1);
        pending.emplace_back(caller);
        auto worker = caller;
        Check(HasPendingWalletTransactions(pending), "in-progress transfer was not fenced");
        caller.reset();
        Check(HasPendingWalletTransactions(pending), "queued database transaction was not fenced");
        worker.reset();
        Check(!HasPendingWalletTransactions(pending) && pending.empty(), "completed transfer fence retained");
        std::uint32_t full = 2147483647;
        Check(CreditDelveOnce({103, 7, 42, 1}, full, blocked, read, commit) == AwardStatus::Deferred && writes == 3,
            "full wallet consumed award");
        Check(CreditDelveOnce({0, 7, 42, 3}, cache, blocked, read, commit) == AwardStatus::Rejected, "zero run accepted");
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--sql")
        {
            std::cout << DelveAwardStatement({100, 7, 42, 3}, 13, 1) << ";\n";
            return 0;
        }
        TestAwards();
        std::cout << "durable award retry, cap, identity and lost-ack recovery passed\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
