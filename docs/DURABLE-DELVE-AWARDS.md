# Durable delve wallet awards

This optional API is a prerequisite for delve rewards; no delve currently calls it.
`BossCoins.DelveAwards.Enable` defaults to zero. It does not enable payouts by itself.
The caller must validate completion and retain an immutable run/member/amount outbox.
Call `Wallet::AwardDelveOnce` with a real online player, stable nonzero run ID and
positive whole award. Self-Found characters are rejected to preserve their personal
bag-coin rules. Their separate reward route remains unfinished.

The receipt key is `delve:<run>:<character>`. A matching receipt returns AlreadyPaid;
changing its account or amount is rejected. Never recycle run IDs. The stored
procedure locks the wallet row and commits receipt, balance delta and ledger together.
It explicitly checks signed-INT capacity even under non-strict MySQL. The cache changes
only after a matching durable receipt is read back. Paid and AlreadyPaid acknowledge
delivery; Deferred should retry later; Rejected requires investigation. Uncertain
must retain the outbox and stop retries for that account until controlled recovery.

Existing bag/shop transaction lifetimes are tracked with weak references, including
caller compensation and queued database workers. Awards defer while these are active.
They also defer if durable balance differs from cache. This does not repair existing
legacy transfer failures or establish general crash safety for those operations.

An uncertain acknowledgement blocks all wallet mutations for that account. Investigate
receipt, ledger, wallet and completion outbox together; a separately authorized clean
startup reloads the durable balance and allows receipt-based retry. Do not delete a
receipt to retry an award or manufacture an opposite adjustment from an unknown result.
The global wallet lock remains held during synchronous SQL; this development API assumes
one worldserver cache owner and no external wallet writers. Measure latency before wider
use. It is not a general shared-database payment endpoint.

## Installation and rollback

On an isolated test realm, with database backup and separate authorization to stop the
realm/apply SQL, install the normal coin schema and
`data/sql/db-characters/mod_boss_coins_delve_awards.sql` in the characters database.
The wallet, ledger and receipt tables must all be InnoDB. Routine creation privileges
are required. Reapplying replaces the procedure while preserving receipts. Startup
checks the schema engines, routine version and loaded balance count before arming.
Enable the feature through the documented server overlay only after a reviewed caller
and compatible server pin are available. No live SQL or restart was performed here.

Rollback disables the feature and reverts the module through the normal reviewed pin
workflow. Preserve receipt, wallet and ledger data together in backups forever: dropping
receipts permits duplicate payouts. Do not run destructive test fixtures against a realm.
The CI fixture uses only its disposable `coin_award_test` database.

## Validation

Standalone sanitizer tests cover duplicates, changed recipients/amounts, cache capacity,
missing acknowledgement, restart recovery and transaction-lifetime fencing. CI applies
the actual schema/procedure twice against MySQL 8, tests injected ledger failure and
strict/non-strict overflow, and runs a persistent C API connection using the core's
`mysql_query` pattern to check that CALL leaves no result sets pending. Every module
translation unit is compiled against the pinned real core headers. In-game behavior,
combined-module linkage and completion outbox integration remain separate requirements.
