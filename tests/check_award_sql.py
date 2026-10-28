#!/usr/bin/env python3
"""Run receipt/wallet/ledger transaction tests in the disposable CI service."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    args = parser.parse_args()
    command = ['mysql', '--protocol=tcp', '-h', '127.0.0.1', '-uroot', '--batch', '--skip-column-names', 'coin_award_test']

    def sql(text, success=True):
        result = subprocess.run(command, input=text, text=True, capture_output=True)
        if (result.returncode == 0) != success:
            raise AssertionError(result.stderr)
        return result.stdout.strip()

    root = Path(__file__).resolve().parents[1]
    schema = (root / 'data/sql/db-characters/mod_boss_coins_characters.sql').read_text()
    receipts = (root / 'data/sql/db-characters/mod_boss_coins_delve_awards.sql').read_text()
    sql(schema)
    sql(receipts)
    sql(receipts)
    award = subprocess.check_output([str(args.binary.resolve()), '--sql'], text=True)
    sql('INSERT INTO mod_boss_coins_wallet(account,coins) VALUES (7,10);')
    sql(award)
    assert sql('SELECT coins FROM mod_boss_coins_wallet WHERE account=7;') == '13'
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_delve_receipt;') == '1'
    assert sql("SELECT COUNT(*) FROM mod_boss_coins_ledger WHERE reason='delve';") == '1'
    sql(award, False)
    assert sql('SELECT coins FROM mod_boss_coins_wallet WHERE account=7;') == '13'
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_ledger;') == '1'

    # Force a failure after receipt and balance writes; all three tables roll back.
    sql('DELETE FROM mod_boss_coins_delve_receipt; DELETE FROM mod_boss_coins_ledger; UPDATE mod_boss_coins_wallet SET coins=10;')
    sql("CREATE TRIGGER refuse_award BEFORE INSERT ON mod_boss_coins_ledger FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected ledger failure';")
    sql(award, False)
    assert sql('SELECT coins FROM mod_boss_coins_wallet WHERE account=7;') == '10'
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_delve_receipt;') == '0'
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_ledger;') == '0'
    sql('DROP TRIGGER refuse_award;')

    # A duplicate key with a different recipient cannot credit its wallet.
    sql(award)
    mismatch = award.replace("100,7,42,3", "100,8,42,3")
    sql(mismatch, False)
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_wallet WHERE account=8;') == '0'
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_ledger;') == '1'

    # Signed-INT overflow cannot leave a consumed receipt or a ledger entry.
    sql('DELETE FROM mod_boss_coins_delve_receipt; DELETE FROM mod_boss_coins_ledger; UPDATE mod_boss_coins_wallet SET coins=2147483647;')
    sql(award, False)
    # Stored routines retain their creation SQL mode, so recreate it non-strict.
    sql("SET SESSION sql_mode='';\n" + receipts)
    sql("SET SESSION sql_mode='';\n" + award, False)
    assert sql('SELECT COUNT(*) FROM mod_boss_coins_delve_receipt;') == '0'
    assert sql('SELECT coins FROM mod_boss_coins_wallet WHERE account=7;') == '2147483647'
    print('Actual generated receipt/wallet/ledger atomicity, duplicate, recipient and overflow checks passed')


if __name__ == '__main__':
    main()
