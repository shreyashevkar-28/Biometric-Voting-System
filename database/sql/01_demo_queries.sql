-- ============================================================================
-- 01_demo_queries.sql : DBMS showcase queries over the voting database
-- Prepare data first:   ./voting_backend --db database/voting.db --simulate 400 --seed-only
-- Run:                  sqlite3 -header -column database/voting.db < database/sql/01_demo_queries.sql
-- (needs SQLite >= 3.25 for window functions)
-- ============================================================================
PRAGMA foreign_keys = ON;

-- Q1  JOIN + GROUP BY + ORDER BY: tally per candidate (LEFT JOIN keeps zero-vote candidates)
SELECT c.name AS candidate, COALESCE(p.name, 'Independent') AS party, COUNT(v.id) AS votes
FROM candidates c
LEFT JOIN parties p ON p.party_id = c.party_id
LEFT JOIN votes   v ON v.candidate_id = c.id
GROUP BY c.id
ORDER BY votes DESC;

-- Q2  Scalar subquery: vote share of each candidate
SELECT candidate, votes,
       ROUND(100.0 * votes / (SELECT COUNT(*) FROM votes), 1) AS share_pct
FROM v_leaderboard;

-- Q3  GROUP BY ... HAVING: constituencies with turnout below 75 %
SELECT k.name AS constituency,
       COUNT(*)                                  AS registered,
       SUM(v.has_voted)                          AS voted,
       ROUND(100.0 * SUM(v.has_voted) / COUNT(*), 1) AS turnout_pct
FROM voters v JOIN constituencies k ON k.constituency_id = v.constituency_id
GROUP BY k.constituency_id
HAVING turnout_pct < 75
ORDER BY turnout_pct;

-- Q4  Uncorrelated subquery: candidates doing better than the average candidate
SELECT candidate, votes FROM v_leaderboard
WHERE votes > (SELECT AVG(votes) FROM v_leaderboard);

-- Q5  Window functions with PARTITION BY: rank inside each party + party subtotal
SELECT party, candidate, votes,
       RANK()      OVER (PARTITION BY party ORDER BY votes DESC) AS rank_in_party,
       SUM(votes)  OVER (PARTITION BY party)                     AS party_total
FROM v_leaderboard
ORDER BY party, rank_in_party;

-- Q6  CTE + running total (view v_hourly_turnout): votes per hour and cumulative
SELECT * FROM v_hourly_turnout;

-- Q7  CTE + subquery: the peak voting hour
WITH h AS (SELECT * FROM v_hourly_turnout)
SELECT hour_utc, votes AS peak_votes FROM h WHERE votes = (SELECT MAX(votes) FROM h);

-- Q8  Set operation (EXCEPT): constituencies where nobody has voted yet (empty = good turnout)
SELECT name FROM constituencies
WHERE constituency_id IN (SELECT constituency_id FROM voters
                          EXCEPT
                          SELECT constituency_id FROM voters WHERE has_voted = 1);

-- Q9  Pure-SQL integrity check with LAG(): number of broken hash links (must be 0)
SELECT COUNT(*) AS broken_links FROM v_chain_links WHERE link_ok = 0;

-- Q10 Invariant: voters flagged as voted == ballots in the ledger (one person, one vote)
SELECT (SELECT COUNT(*) FROM voters WHERE has_voted = 1) AS voters_flagged,
       (SELECT COUNT(*) FROM votes)                      AS ballots,
       (SELECT COUNT(*) FROM voters WHERE has_voted = 1) = (SELECT COUNT(*) FROM votes) AS invariant_holds;

-- Q11 Ballot-secrecy proof from the catalog: `votes` has no voter column and its only FK is candidates.
SELECT name AS votes_column FROM pragma_table_info('votes');
SELECT "table" AS referenced_table, "from" AS column_name FROM pragma_foreign_key_list('votes');

-- Q12 Trigger-fed audit trail summary (note: BALLOT_RECORDED rows carry NO voter id)
SELECT event_type, COUNT(*) AS events FROM audit_log GROUP BY event_type;

-- ---------------------------------------------------------------------------
-- Index / query-plan experiments (EXPLAIN QUERY PLAN)
-- ---------------------------------------------------------------------------
-- E1  Primary-key lookup: the table IS the B-Tree on sensor_id  -> "SEARCH voters USING INTEGER PRIMARY KEY"
EXPLAIN QUERY PLAN SELECT * FROM voters WHERE sensor_id = 42;

-- E2  Range query on the time index -> "SEARCH votes USING COVERING INDEX idx_votes_time"
EXPLAIN QUERY PLAN SELECT COUNT(*) FROM votes WHERE timestamp_ms >= 1700000000000 AND timestamp_ms < 1800000000000;

-- E3  Partial index (only rows with has_voted = 0) -> "USING INDEX idx_voters_remaining"
EXPLAIN QUERY PLAN SELECT COUNT(*) FROM voters WHERE has_voted = 0 AND constituency_id = 3;

-- E4  Join on an FK index -> candidate tally uses idx_votes_candidate
EXPLAIN QUERY PLAN SELECT c.name, COUNT(*) FROM candidates c JOIN votes v ON v.candidate_id = c.id GROUP BY c.id;

-- E5  Before/after: no index on voters.name, so a name search scans the whole table...
EXPLAIN QUERY PLAN SELECT * FROM voters WHERE name = 'Aarav Patil #1';
BEGIN;                                   -- DDL is transactional in SQLite, so this experiment leaves no trace
CREATE INDEX tmp_voters_name ON voters(name);
EXPLAIN QUERY PLAN SELECT * FROM voters WHERE name = 'Aarav Patil #1';   -- ...now "SEARCH ... USING INDEX tmp_voters_name"
ROLLBACK;
-- (The application's name search uses a Trie instead; see docs/DBMS_AND_ADS_MAPPING.md.)
