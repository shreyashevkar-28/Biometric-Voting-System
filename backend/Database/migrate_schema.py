import sqlite3

def migrate(db_path='database/voting.db'):
    con = sqlite3.connect(db_path)
    cur = con.cursor()

    # Create new ER diagram tables if not exist
    cur.executescript("""
    CREATE TABLE IF NOT EXISTS persons (
        person_id INTEGER PRIMARY KEY AUTOINCREMENT,
        name      TEXT NOT NULL CHECK (length(name) BETWEEN 1 AND 64),
        dob       TEXT NOT NULL,
        email     TEXT CHECK (email IS NULL OR length(email) <= 128),
        address   TEXT,
        gender    TEXT CHECK (gender IS NULL OR gender IN ('Male', 'Female', 'Other'))
    );

    CREATE TABLE IF NOT EXISTS elections (
        election_id INTEGER PRIMARY KEY AUTOINCREMENT,
        type        TEXT NOT NULL DEFAULT 'General',
        date        TEXT NOT NULL DEFAULT (date('now')),
        st_time     TEXT NOT NULL DEFAULT '08:00',
        end_time    TEXT NOT NULL DEFAULT '18:00',
        status      TEXT NOT NULL DEFAULT 'registration' CHECK (status IN ('registration', 'polling', 'closed'))
    );

    CREATE TABLE IF NOT EXISTS polling_booths (
        booth_id        INTEGER PRIMARY KEY AUTOINCREMENT,
        constituency_id INTEGER REFERENCES constituencies(constituency_id) ON DELETE RESTRICT,
        capacity        INTEGER NOT NULL DEFAULT 1000 CHECK (capacity > 0)
    );

    CREATE TABLE IF NOT EXISTS officers (
        officer_id INTEGER PRIMARY KEY AUTOINCREMENT,
        name       TEXT NOT NULL,
        phone      TEXT,
        booth_id   INTEGER REFERENCES polling_booths(booth_id)
    );

    CREATE TABLE IF NOT EXISTS biometric_devices (
        device_id  TEXT PRIMARY KEY,
        booth_id   INTEGER REFERENCES polling_booths(booth_id) ON DELETE RESTRICT,
        officer_id INTEGER REFERENCES officers(officer_id),
        status     TEXT NOT NULL DEFAULT 'active' CHECK (status IN ('active', 'inactive', 'maintenance'))
    );

    CREATE TABLE IF NOT EXISTS election_constituencies (
        election_id     INTEGER REFERENCES elections(election_id) ON DELETE CASCADE,
        constituency_id INTEGER REFERENCES constituencies(constituency_id) ON DELETE CASCADE,
        PRIMARY KEY (election_id, constituency_id)
    );

    CREATE TABLE IF NOT EXISTS election_candidates (
        election_id  INTEGER REFERENCES elections(election_id) ON DELETE CASCADE,
        candidate_id INTEGER REFERENCES candidates(id) ON DELETE CASCADE,
        PRIMARY KEY (election_id, candidate_id)
    );
    """)

    def col_exists(table, col):
        return any(row[1] == col for row in cur.execute(f"PRAGMA table_info({table})"))

    # Alter existing tables if needed
    if not col_exists('candidates', 'symbol'):
        cur.execute("ALTER TABLE candidates ADD COLUMN symbol TEXT DEFAULT '🗳️'")
    if not col_exists('candidates', 'constituency_id'):
        cur.execute("ALTER TABLE candidates ADD COLUMN constituency_id INTEGER REFERENCES constituencies(constituency_id)")
    if not col_exists('parties', 'symbol'):
        cur.execute("ALTER TABLE parties ADD COLUMN symbol TEXT DEFAULT '🚩'")
    if not col_exists('constituencies', 'state'):
        cur.execute("ALTER TABLE constituencies ADD COLUMN state TEXT DEFAULT 'Maharashtra'")
    if not col_exists('voters', 'person_id'):
        cur.execute("ALTER TABLE voters ADD COLUMN person_id INTEGER REFERENCES persons(person_id)")
    if not col_exists('voters', 'dob'):
        cur.execute("ALTER TABLE voters ADD COLUMN dob TEXT")
    if not col_exists('voters', 'gender'):
        cur.execute("ALTER TABLE voters ADD COLUMN gender TEXT")
    if not col_exists('voters', 'phone'):
        cur.execute("ALTER TABLE voters ADD COLUMN phone TEXT")
    if not col_exists('voters', 'address'):
        cur.execute("ALTER TABLE voters ADD COLUMN address TEXT")
    if not col_exists('votes', 'election_id'):
        cur.execute("ALTER TABLE votes ADD COLUMN election_id INTEGER REFERENCES elections(election_id) DEFAULT 1")
    if not col_exists('votes', 'machine_id'):
        cur.execute("ALTER TABLE votes ADD COLUMN machine_id TEXT REFERENCES biometric_devices(device_id) DEFAULT 'DEV-01'")

    # Ensure default election and constituencies exist if empty
    if cur.execute("SELECT COUNT(*) FROM elections").fetchone()[0] == 0:
        cur.execute("INSERT INTO elections(election_id, type, date, st_time, end_time, status) VALUES(1, 'General Election 2026', date('now'), '08:00', '18:00', 'registration')")

    if cur.execute("SELECT COUNT(*) FROM constituencies").fetchone()[0] == 0:
        for cname in ['Ahilyanagar Central', 'Ahilyanagar North', 'Ahilyanagar South']:
            cur.execute("INSERT INTO constituencies(name, state) VALUES(?, 'Maharashtra')", (cname,))

    if cur.execute("SELECT COUNT(*) FROM polling_booths").fetchone()[0] == 0:
        cur.execute("INSERT INTO polling_booths(booth_id, constituency_id, capacity) VALUES(1, 1, 1200)")

    if cur.execute("SELECT COUNT(*) FROM officers").fetchone()[0] == 0:
        cur.execute("INSERT INTO officers(officer_id, name, phone, booth_id) VALUES(1, 'Rajesh Sharma (Presiding Officer)', '9822011223', 1)")

    if cur.execute("SELECT COUNT(*) FROM biometric_devices").fetchone()[0] == 0:
        cur.execute("INSERT INTO biometric_devices(device_id, booth_id, officer_id, status) VALUES('DEV-01', 1, 1, 'active')")

    # Update candidate symbols if default or null
    cand_symbols = {'ALICE': '🌾', 'BOB': '🪷', 'CAROL': '🚲'}
    for cname, sym in cand_symbols.items():
        cur.execute("UPDATE candidates SET symbol = ? WHERE UPPER(name) = ? AND (symbol IS NULL OR symbol = '🗳️')", (sym, cname))

    con.commit()
    con.close()
    print("Migration completed successfully!")

if __name__ == '__main__':
    migrate()
