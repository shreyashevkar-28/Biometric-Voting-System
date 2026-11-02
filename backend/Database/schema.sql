-- ============================================================================
-- Biometric Voting System : Relational Schema aligned with ER Diagram
-- Entities: Person, Voter, Candidate, Party, Constituency, Election,
--           Polling_Booth, Biometric_Device, Officer, Vote, Biometric_Templates
-- Relationships:
--   Person -- ISA --> Voter
--   Voter -- Registered_IN --> Constituency
--   Candidate -- belongs_to --> Party
--   Candidate -- stands_from --> Constituency
--   Election -- conducted_In --> Constituency (M:N)
--   Election -- contests --> Candidate (M:N)
--   Polling_Booth -- Located_in --> Constituency
--   Officer -- manages --> Polling_Booth
--   Biometric_Device -- Installed_At --> Polling_Booth
--   Officer -- Supervises --> Biometric_Device
--   Vote -- Recorded_for --> Candidate
--   Vote -- Records_for --> Election
--   Vote -- Authenticates --> Biometric_Device
--   Voter -- Casts --> Vote (1:1 monotone flag `has_voted`, append-only secret ballot)
-- ============================================================================

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- ---------------------------------------------------------------- reference & organization
CREATE TABLE IF NOT EXISTS constituencies (
    constituency_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(name) BETWEEN 1 AND 64),
    state           TEXT NOT NULL DEFAULT 'Maharashtra' CHECK (length(state) BETWEEN 1 AND 64)
);

CREATE TABLE IF NOT EXISTS parties (
    party_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name     TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(name) BETWEEN 1 AND 64),
    symbol   TEXT DEFAULT '🚩' CHECK (length(symbol) <= 32)
);

-- ---------------------------------------------------------------- elections
CREATE TABLE IF NOT EXISTS elections (
    election_id INTEGER PRIMARY KEY AUTOINCREMENT,
    type        TEXT NOT NULL DEFAULT 'General Election' CHECK (length(type) BETWEEN 1 AND 64),
    date        TEXT NOT NULL DEFAULT (date('now')),
    st_time     TEXT NOT NULL DEFAULT '08:00',
    end_time    TEXT NOT NULL DEFAULT '18:00',
    status      TEXT NOT NULL DEFAULT 'registration' CHECK (status IN ('registration', 'polling', 'closed'))
);

CREATE TABLE IF NOT EXISTS election_constituencies (
    election_id     INTEGER REFERENCES elections(election_id) ON DELETE CASCADE,
    constituency_id INTEGER REFERENCES constituencies(constituency_id) ON DELETE CASCADE,
    PRIMARY KEY (election_id, constituency_id)
);

-- ---------------------------------------------------------------- polling booths & hardware
CREATE TABLE IF NOT EXISTS polling_booths (
    booth_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    constituency_id INTEGER REFERENCES constituencies(constituency_id) ON DELETE RESTRICT,
    capacity        INTEGER NOT NULL DEFAULT 1000 CHECK (capacity > 0)
);

CREATE TABLE IF NOT EXISTS officers (
    officer_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
    phone      TEXT CHECK (phone IS NULL OR length(phone) <= 20),
    booth_id   INTEGER REFERENCES polling_booths(booth_id)
);

CREATE TABLE IF NOT EXISTS biometric_devices (
    device_id  TEXT PRIMARY KEY,
    booth_id   INTEGER REFERENCES polling_booths(booth_id) ON DELETE RESTRICT,
    officer_id INTEGER REFERENCES officers(officer_id),
    status     TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'inactive', 'maintenance'))
);

-- ---------------------------------------------------------------- persons & voters (ISA)
CREATE TABLE IF NOT EXISTS persons (
    person_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name      TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
    dob       TEXT NOT NULL,
    age       INTEGER GENERATED ALWAYS AS (CAST((strftime('%Y', 'now') - strftime('%Y', dob)) - (strftime('%m-%d', 'now') < strftime('%m-%d', dob)) AS INTEGER)) VIRTUAL,
    email     TEXT CHECK (email IS NULL OR length(email) <= 128),
    address   TEXT,
    gender    TEXT CHECK (gender IS NULL OR gender IN ('Male', 'Female', 'Other'))
);

CREATE TABLE IF NOT EXISTS voters (
    sensor_id       INTEGER PRIMARY KEY CHECK (sensor_id BETWEEN 0 AND 65535),   -- R307S slot (B-Tree PK)
    person_id       INTEGER REFERENCES persons(person_id) ON DELETE CASCADE,
    name            TEXT    NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
    dob             TEXT,
    gender          TEXT,
    phone           TEXT,
    address         TEXT,
    constituency_id INTEGER REFERENCES constituencies(constituency_id)
                            ON DELETE RESTRICT ON UPDATE CASCADE,
    has_voted       INTEGER NOT NULL DEFAULT 0 CHECK (has_voted IN (0, 1)),
    enrolled_at_ms  INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    voter_ref       TEXT CHECK (voter_ref IS NULL OR length(voter_ref) BETWEEN 1 AND 32)   -- portal Voter ID (e.g. VTR100001)
);

-- Fingerprint templates captured from the R307S during enrolment. Vertical partition (512-byte BLOB).
-- Kept intact even when polls are reset.
CREATE TABLE IF NOT EXISTS biometric_templates (
    sensor_id       INTEGER PRIMARY KEY REFERENCES voters(sensor_id) ON DELETE CASCADE,
    template        BLOB    NOT NULL CHECK (length(template) BETWEEN 64 AND 2048),
    template_sha256 TEXT    NOT NULL CHECK (length(template_sha256) = 64),
    template_bytes  INTEGER NOT NULL,
    finger          TEXT    NOT NULL DEFAULT 'Unspecified' CHECK (length(finger) <= 32),
    enrolled_at_ms  INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);

-- ---------------------------------------------------------------- candidates
CREATE TABLE IF NOT EXISTS candidates (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    name            TEXT NOT NULL UNIQUE COLLATE NOCASE CHECK (length(name) BETWEEN 1 AND 64),
    symbol          TEXT NOT NULL DEFAULT '🗳️' CHECK (length(symbol) <= 32),
    party_id        INTEGER REFERENCES parties(party_id) ON DELETE RESTRICT ON UPDATE CASCADE,  -- NULL = independent
    constituency_id INTEGER REFERENCES constituencies(constituency_id) ON DELETE RESTRICT
);

CREATE TABLE IF NOT EXISTS election_candidates (
    election_id  INTEGER REFERENCES elections(election_id) ON DELETE CASCADE,
    candidate_id INTEGER REFERENCES candidates(id) ON DELETE CASCADE,
    PRIMARY KEY (election_id, candidate_id)
);

-- ---------------------------------------------------------------- votes ledger
-- Append-only, hash-chained. NO voter column anywhere: ballot secrecy by schema design.
CREATE TABLE IF NOT EXISTS votes (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    election_id    INTEGER REFERENCES elections(election_id) DEFAULT 1,
    candidate_id   INTEGER NOT NULL REFERENCES candidates(id) ON DELETE RESTRICT,
    candidate_name TEXT    NOT NULL,
    machine_id     TEXT    REFERENCES biometric_devices(device_id) DEFAULT 'DEV-01',
    timestamp_ms   INTEGER NOT NULL,                              -- unix epoch ms, UTC
    previous_hash  TEXT    NOT NULL CHECK (length(previous_hash) = 64),
    current_hash   TEXT    NOT NULL UNIQUE CHECK (length(current_hash) = 64)
    -- current_hash = SHA256(candidate_name || '|' || timestamp_ms || '|' || previous_hash)
);

-- Administrative event log, filled ONLY by triggers (never by application code) + admin reset audit.
CREATE TABLE IF NOT EXISTS audit_log (
    event_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    ts_ms      INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    event_type TEXT    NOT NULL,
    detail     TEXT    NOT NULL
);

-- ---------------------------------------------------------------- indexes
CREATE INDEX IF NOT EXISTS idx_votes_candidate     ON votes(candidate_id);
CREATE INDEX IF NOT EXISTS idx_votes_time          ON votes(timestamp_ms);
CREATE INDEX IF NOT EXISTS idx_voters_constituency  ON voters(constituency_id);
CREATE INDEX IF NOT EXISTS idx_voters_remaining    ON voters(constituency_id) WHERE has_voted = 0;
CREATE INDEX IF NOT EXISTS idx_audit_time          ON audit_log(ts_ms);
CREATE UNIQUE INDEX IF NOT EXISTS idx_voters_ref   ON voters(voter_ref) WHERE voter_ref IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_candidates_party    ON candidates(party_id);
CREATE INDEX IF NOT EXISTS idx_candidates_const    ON candidates(constituency_id);

-- ---------------------------------------------------------------- integrity triggers
CREATE TRIGGER IF NOT EXISTS votes_no_update BEFORE UPDATE ON votes
BEGIN SELECT RAISE(ABORT, 'votes ledger is append-only'); END;

-- Poll reset is strictly guarded: deletion allowed ONLY when allow_election_reset = '1' in meta
CREATE TRIGGER IF NOT EXISTS votes_no_delete BEFORE DELETE ON votes
WHEN (SELECT COALESCE(value, '0') FROM meta WHERE key = 'allow_election_reset') <> '1'
BEGIN SELECT RAISE(ABORT, 'votes ledger is append-only; cannot delete ballots outside authorized poll reset'); END;

-- Hash chain is enforced inside the database
CREATE TRIGGER IF NOT EXISTS votes_chain_guard BEFORE INSERT ON votes
WHEN NEW.previous_hash <> COALESCE((SELECT current_hash FROM votes ORDER BY id DESC LIMIT 1),
                                   '0000000000000000000000000000000000000000000000000000000000000000')
BEGIN SELECT RAISE(ABORT, 'hash chain break: previous_hash does not match ledger head'); END;

-- Candidate name snapshot must equal the candidate it points at
CREATE TRIGGER IF NOT EXISTS votes_name_guard BEFORE INSERT ON votes
WHEN NOT EXISTS (SELECT 1 FROM candidates c
                 WHERE c.id = NEW.candidate_id AND c.name = NEW.candidate_name COLLATE BINARY)
BEGIN SELECT RAISE(ABORT, 'candidate_name does not match candidate_id'); END;

-- Monotone voting flag: reset allowed ONLY during authorized election reset
CREATE TRIGGER IF NOT EXISTS voters_no_unvote BEFORE UPDATE OF has_voted ON voters
WHEN OLD.has_voted = 1 AND NEW.has_voted = 0
  AND (SELECT COALESCE(value, '0') FROM meta WHERE key = 'allow_election_reset') <> '1'
BEGIN SELECT RAISE(ABORT, 'has_voted cannot be reset outside authorized poll reset'); END;

CREATE TRIGGER IF NOT EXISTS voters_no_delete_voted BEFORE DELETE ON voters
WHEN OLD.has_voted = 1
  AND (SELECT COALESCE(value, '0') FROM meta WHERE key = 'allow_election_reset') <> '1'
BEGIN SELECT RAISE(ABORT, 'cannot delete a voter who has voted'); END;

-- ---------------------------------------------------------------- audit triggers
CREATE TRIGGER IF NOT EXISTS trg_audit_voter AFTER INSERT ON voters
BEGIN INSERT INTO audit_log(event_type, detail) VALUES ('VOTER_ENROLLED', 'sensor_id=' || NEW.sensor_id); END;

CREATE TRIGGER IF NOT EXISTS trg_audit_candidate AFTER INSERT ON candidates
BEGIN INSERT INTO audit_log(event_type, detail) VALUES ('CANDIDATE_ADDED', 'candidate_id=' || NEW.id || ' name=' || NEW.name); END;

CREATE TRIGGER IF NOT EXISTS trg_audit_ballot AFTER INSERT ON votes
BEGIN INSERT INTO audit_log(event_type, detail)
      VALUES ('BALLOT_RECORDED', 'vote_id=' || NEW.id || ' hash=' || substr(NEW.current_hash, 1, 12)); END;

CREATE TRIGGER IF NOT EXISTS audit_no_update BEFORE UPDATE ON audit_log
BEGIN SELECT RAISE(ABORT, 'audit_log is append-only'); END;

CREATE TRIGGER IF NOT EXISTS audit_no_delete BEFORE DELETE ON audit_log
WHEN (SELECT COALESCE(value, '0') FROM meta WHERE key = 'allow_election_reset') <> '1'
BEGIN SELECT RAISE(ABORT, 'audit_log is append-only'); END;

-- ---------------------------------------------------------------- views
-- Derived attributes from ER Diagram: vote_count (COUNT(v.id)) and Result-status (Leading / Trailing)
CREATE VIEW IF NOT EXISTS v_leaderboard AS
SELECT RANK() OVER (ORDER BY COUNT(v.id) DESC) AS position,
       c.id                                    AS candidate_id,
       c.name                                  AS candidate,
       COALESCE(c.symbol, '🗳️')                 AS symbol,
       COALESCE(p.name, 'Independent')         AS party,
       COALESCE(k.name, 'General')             AS constituency,
       COUNT(v.id)                             AS votes,
       CASE 
           WHEN COUNT(v.id) = 0 THEN 'Pending'
           WHEN COUNT(v.id) = (SELECT MAX(cnt) FROM (SELECT COUNT(v2.id) as cnt FROM candidates c2 LEFT JOIN votes v2 ON v2.candidate_id = c2.id GROUP BY c2.id)) THEN 'Leading / Elected'
           ELSE 'Trailing'
       END                                     AS result_status
FROM candidates c
LEFT JOIN parties p ON p.party_id = c.party_id
LEFT JOIN constituencies k ON k.constituency_id = c.constituency_id
LEFT JOIN votes   v ON v.candidate_id = c.id
GROUP BY c.id
ORDER BY votes DESC, c.id;

CREATE VIEW IF NOT EXISTS v_party_tally AS
SELECT COALESCE(p.name, 'Independent') AS party,
       COALESCE(p.symbol, '🚩')         AS symbol,
       COUNT(DISTINCT c.id)            AS candidates,
       COUNT(v.id)                     AS votes
FROM candidates c
LEFT JOIN parties p ON p.party_id = c.party_id
LEFT JOIN votes   v ON v.candidate_id = c.id
GROUP BY p.party_id
ORDER BY votes DESC;

CREATE VIEW IF NOT EXISTS v_constituency_turnout AS
SELECT k.constituency_id,
       k.name                                              AS constituency,
       k.state,
       COUNT(v.sensor_id)                                  AS registered,
       COALESCE(SUM(v.has_voted), 0)                       AS voted,
       ROUND(100.0 * COALESCE(SUM(v.has_voted), 0) / NULLIF(COUNT(v.sensor_id), 0), 1) AS turnout_pct
FROM constituencies k
LEFT JOIN voters v ON v.constituency_id = k.constituency_id
GROUP BY k.constituency_id
ORDER BY turnout_pct DESC;

CREATE VIEW IF NOT EXISTS v_hourly_turnout AS
WITH h AS (
    SELECT strftime('%Y-%m-%d %H:00', timestamp_ms / 1000, 'unixepoch') AS hour_utc,
           COUNT(*) AS votes
    FROM votes GROUP BY 1
)
SELECT hour_utc, votes, SUM(votes) OVER (ORDER BY hour_utc) AS cumulative
FROM h ORDER BY hour_utc;

CREATE VIEW IF NOT EXISTS v_chain_links AS
SELECT id, previous_hash, expected_prev, (previous_hash = expected_prev) AS link_ok
FROM (
    SELECT id, previous_hash,
           LAG(current_hash, 1, '0000000000000000000000000000000000000000000000000000000000000000')
               OVER (ORDER BY id) AS expected_prev
    FROM votes
);

CREATE VIEW IF NOT EXISTS v_booth_devices AS
SELECT b.booth_id,
       k.name AS constituency,
       b.capacity,
       d.device_id,
       d.status AS device_status,
       o.name AS supervising_officer,
       o.phone AS officer_phone
FROM polling_booths b
LEFT JOIN constituencies k ON k.constituency_id = b.constituency_id
LEFT JOIN biometric_devices d ON d.booth_id = b.booth_id
LEFT JOIN officers o ON o.officer_id = d.officer_id;
