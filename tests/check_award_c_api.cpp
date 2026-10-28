#include "DurableAward.h"
#include <mysql/mysql.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main()
{
    MYSQL* database = mysql_init(nullptr);
    try
    {
        if (!mysql_real_connect(database, "127.0.0.1", "root", std::getenv("MYSQL_PWD"),
            "coin_award_test", 3306, nullptr, 0))
            throw std::runtime_error(mysql_error(database));
        if (mysql_query(database, "UPDATE mod_boss_coins_wallet SET coins=10 WHERE account=7"))
            throw std::runtime_error(mysql_error(database));
        auto call = BossCoins::Wallet::DelveAwardStatement({201, 7, 42, 3}, 13, 1);
        if (mysql_query(database, call.c_str()))
            throw std::runtime_error(mysql_error(database));
        // The core Execute path does not drain result sets. This IN-only routine
        // must therefore produce only its final status, no data result sets.
        if (mysql_field_count(database) || mysql_more_results(database))
            throw std::runtime_error("award routine left results pending on the core-style connection");
        if (!mysql_query(database, call.c_str()) || mysql_errno(database) != 1062)
            throw std::runtime_error("duplicate award did not fail its receipt key");
        if (mysql_more_results(database))
            throw std::runtime_error("failed award left extra results");
        if (mysql_query(database, "SELECT coins FROM mod_boss_coins_wallet WHERE account=7"))
            throw std::runtime_error(mysql_error(database));
        MYSQL_RES* result = mysql_store_result(database);
        MYSQL_ROW row = result ? mysql_fetch_row(result) : nullptr;
        bool valid = row && row[0] && std::string(row[0]) == "13";
        if (result)
            mysql_free_result(result);
        if (!valid)
            throw std::runtime_error("persistent C API connection or wallet result invalid");
        mysql_close(database);
        std::cout << "Core-style mysql_query CALL/readback works without pending result sets\n";
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        if (database)
            mysql_close(database);
        return 1;
    }
}
