-- Durable external delve credits. Retain with wallet and ledger backups forever;
-- deleting a receipt permits a run to pay again. This SQL alone enables no awards.
CREATE TABLE IF NOT EXISTS `mod_boss_coins_delve_receipt` (
  `reward_key` VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `run_id` BIGINT UNSIGNED NOT NULL,
  `account` INT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL,
  `coins` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`reward_key`),
  UNIQUE KEY `run_member` (`run_id`, `guid`)
) ENGINE=InnoDB;

-- Apply while the realm is stopped. Explicit SIGNAL/rollback enforces all-or-none
-- capacity independently of the server/session sql_mode.
DROP PROCEDURE IF EXISTS `mod_boss_coins_delve_award`;
DELIMITER //
CREATE PROCEDURE `mod_boss_coins_delve_award`(
  IN p_run BIGINT UNSIGNED, IN p_account INT UNSIGNED, IN p_guid INT UNSIGNED,
  IN p_coins INT UNSIGNED, IN p_balance INT UNSIGNED, IN p_at BIGINT UNSIGNED)
COMMENT 'delve-award-v1'
BEGIN
  DECLARE actual_balance BIGINT;
  DECLARE award_key VARCHAR(64);
  DECLARE EXIT HANDLER FOR SQLEXCEPTION
  BEGIN
    ROLLBACK;
    RESIGNAL;
  END;
  IF p_run IS NULL OR p_account IS NULL OR p_guid IS NULL OR p_coins IS NULL OR p_balance IS NULL OR p_at IS NULL
    OR p_run = 0 OR p_account = 0 OR p_guid = 0 OR p_coins = 0 OR p_coins > 2147483647 OR p_balance > 2147483647 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Invalid delve award identity or amount';
  END IF;
  SET award_key = CONCAT('delve:',p_run,':',p_guid);
  START TRANSACTION;
  INSERT INTO mod_boss_coins_wallet (account,coins) VALUES (p_account,0)
    ON DUPLICATE KEY UPDATE account = p_account;
  SELECT coins INTO actual_balance FROM mod_boss_coins_wallet WHERE account = p_account FOR UPDATE;
  IF actual_balance < 0 OR actual_balance > 2147483647 - p_coins THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'Insufficient durable wallet capacity';
  END IF;
  INSERT INTO mod_boss_coins_delve_receipt (reward_key,run_id,account,guid,coins)
    VALUES (award_key,p_run,p_account,p_guid,p_coins);
  UPDATE mod_boss_coins_wallet SET coins = coins + p_coins WHERE account = p_account;
  INSERT INTO mod_boss_coins_ledger (at,account,guid,coins,balance,reason,offer)
    VALUES (p_at,p_account,p_guid,p_coins,p_balance,'delve',0);
  COMMIT;
END//
DELIMITER ;
