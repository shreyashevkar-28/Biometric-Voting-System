-- ============================================================================
-- 02_transactions_acid_demo.sql : ACID properties and integrity constraints in action
-- Run on a SCRATCH COPY:  cp database/voting.db /tmp/demo.db && sqlite3 -header -column /tmp/demo.db < database/sql/02_transactions_acid_demo.sql
-- Statements marked  -- EXPECT ERROR  are supposed to fail: that is the point of the demo.
-- (The sqlite3 shell keeps going after an error unless started with -bail.)
-- ============================================================================
PRAGMA foreign_keys = ON;

-- ---- A  Atomicity: a vote is "flag the voter" + "append the ballot"; if the 2nd step fails, the 1st is undone
SELECT 'before' AS stage, (SELECT COUNT(*) FROM voters WHERE has_voted = 1) AS voters_flagged, (SELECT COUNT(*) FROM votes) AS ballots;

BEGIN IMMEDIATE;
UPDATE voters SET has_voted = 1
 WHERE sensor_id = (SELECT MIN(sensor_id) FROM voters WHERE has_voted = 0);
-- EXPECT ERROR: forged chain link rejected by trigger votes_chain_guard
INSERT INTO votes(candidate_id, candidate_name, timestamp_ms, previous_hash, current_hash)
VALUES (1, (SELECT name FROM candidates WHERE id = 1), 1, hex(randomblob(32)), hex(randomblob(32)));
ROLLBACK;                                -- the UPDATE above disappears too

SELECT 'after rollback' AS stage, (SELECT COUNT(*) FROM voters WHERE has_voted = 1) AS voters_flagged, (SELECT COUNT(*) FROM votes) AS ballots;

-- ---- B  Consistency: constraints and triggers reject illegal states
UPDATE votes SET candidate_name = 'MALLORY' WHERE id = 1;          -- EXPECT ERROR: votes ledger is append-only
DELETE FROM votes;                                                  -- EXPECT ERROR: votes ledger is append-only
UPDATE voters SET has_voted = 0 WHERE has_voted = 1;                -- EXPECT ERROR: has_voted cannot be reset
DELETE FROM voters WHERE has_voted = 1;                             -- EXPECT ERROR: cannot delete a voter who has voted
DELETE FROM candidates WHERE id = 1;                                -- EXPECT ERROR: FOREIGN KEY constraint failed
INSERT INTO voters(sensor_id, name) VALUES (70000, 'Out Of Range'); -- EXPECT ERROR: CHECK constraint (sensor_id 0..65535)
INSERT INTO voters(sensor_id, name, constituency_id) VALUES (65000, 'Ghost', 999);  -- EXPECT ERROR: FOREIGN KEY
UPDATE audit_log SET detail = 'edited' WHERE event_id = 1;          -- EXPECT ERROR: audit_log is append-only

-- ---- C  One-person-one-vote at SQL level: the conditional UPDATE changes 0 rows the second time
UPDATE voters SET has_voted = 1 WHERE sensor_id = (SELECT MIN(sensor_id) FROM voters WHERE has_voted = 1) AND has_voted = 0;
SELECT changes() AS rows_changed_on_double_vote;                    -- 0 -> the application treats this as DENY

-- ---- D  Isolation & Durability settings the backend relies on
PRAGMA journal_mode;      -- 'wal': readers see a consistent snapshot and never block the single writer
PRAGMA synchronous;       -- 2 = FULL on the backend: fsync before COMMIT returns, so an acknowledged vote survives power loss
PRAGMA foreign_keys;      -- 1
PRAGMA integrity_check;   -- 'ok' = B-Tree pages are structurally sound

-- ---- E  Savepoints: partial rollback inside a transaction
BEGIN;
SAVEPOINT sp1;
INSERT INTO parties(name) VALUES ('Temporary Party');
SELECT COUNT(*) AS parties_inside_txn FROM parties;
ROLLBACK TO sp1;
SELECT COUNT(*) AS parties_after_savepoint_rollback FROM parties;
ROLLBACK;
