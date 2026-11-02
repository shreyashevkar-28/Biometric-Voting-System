# Biometric-Voting-System

## C++ backend
PC-tethered biometric voting. The Arduino UNO + R307S is a dumb terminal speaking
`SCAN:<ID>` / `VOTE:<CANDIDATE>` over USB; this backend does all authentication, state,
concurrency and storage.

```
BiometricVotingSystem/
├── docs/                     ER_DIAGRAM.md, NORMALIZATION.md, DBMS_AND_ADS_MAPPING.md  <- DBMS course material
├── frontend/                 Vanilla JS portal (admin + voter), served at /
├── firmware/VotingTerminal/  Arduino UNO sketch: voting terminal + fingerprint enrolment
├── backend/
│   ├── main.cpp              boot, wiring, HTTP server, graceful shutdown
│   ├── CMakeLists.txt
│   ├── API/                  AuthController, VoteController, ResultController (+ ApiCommon.h)
│   ├── DataStructures/       Trie, MaxHeap, SegmentTree, SkipList  (from scratch)
│   ├── Models/               Voter, Candidate, Vote
│   ├── Database/             DatabaseManager + schema.sql
│   ├── Services/             AuthService, VotingService, ResultService, EnrollmentService
│   ├── Hardware/             SerialListener (reader thread + worker thread)
│   ├── Tools/                DemoSeeder (--simulate N)
│   ├── Common/               Crypto.h (own SHA-256), Util.h
│   └── tests/                test_core.cpp (data structures + SHA-256), e2e_enroll.py (enrolment, simulated sensor)
└── database/
    ├── sql/01_demo_queries.sql           JOIN/GROUP BY/HAVING/subquery/CTE/window/EXPLAIN showcase
    ├── sql/02_transactions_acid_demo.sql atomicity, constraints, triggers, savepoints, PRAGMAs
    └── voting.db                         created at first run
```

## Build
Requires a C++17 compiler, CMake >= 3.16, SQLite3 dev files. cpp-httplib is fetched by CMake.

```bash
# Ubuntu/Debian: sudo apt install build-essential cmake libsqlite3-dev
# Windows: vcpkg install sqlite3  (then pass -DCMAKE_TOOLCHAIN_FILE=...vcpkg.cmake)
cd backend
cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # core tests
```

## Run
```bash
export VOTING_ADMIN_TOKEN=choose-a-long-random-secret     # PowerShell: $env:VOTING_ADMIN_TOKEN="..."
./build/voting_backend --serial /dev/ttyACM0 --seed-demo   # Windows: --serial COM3
```
Binds to **127.0.0.1:8080 only**. Other options: `--port --db --schema --frontend --baud --no-serial --window-hours`.
`--seed-demo` creates candidates ALICE, BOB, CAROL (names must match `CANDIDATE_NAMES` in the Arduino sketch;
`VOTE:ALICE_SMITH` matches a candidate named "Alice Smith").
Close the Arduino Serial Monitor first (it holds the port).

## Fingerprint enrolment (live sensor -> database)
Enrolment is part of **registration**: on `http://127.0.0.1:8080` choose *New registration*, fill the form, pick a finger and press
*Submit and scan fingerprint*. The application is only created once the fingerprint is safely in the database, and an officer
can approve a voter only if a fingerprint is on record. (A voter whose scan failed can retry under **Biometric status**, and an
officer can enrol from **Admin -> Biometric enrolment**.) The backend tells the UNO to start; the R307S's light comes on and the screen
walks the voter through it:

1. place finger (scan 1) -> sensor searches its flash: **a finger that is already enrolled is refused** (one person, one ballot)
2. lift, place the same finger again (scan 2) -> sensor merges both into one template and stores it in the next free slot
3. the UNO reads the 512-byte template back out of the sensor and streams it to the PC (24-byte hex lines, CRC-16 checked)
4. the backend verifies length + CRC, SHA-256s it, and in **one SQL transaction** inserts the `voters` row
   (`sensor_id` = sensor slot, `voter_ref` = portal Voter ID) and the `biometric_templates` row (BLOB + hash + finger label)

Any failure (timeout, mismatch, duplicate, bad CRC, database error) leaves **no** half-state: the UNO deletes its own slot
and the backend sends `DEL:<slot>` if it was the one that failed. Enrolment closes automatically once the first ballot exists.

**During the election** the sensor still does the matching (1:N search of its flash) and sends `SCAN:<slot>`; the backend
admits that slot only if its `voters` row exists and `has_voted = 0`. The template stored in `biometric_templates` is the
authoritative copy of what the sensor holds (audit, and re-flashing a replaced sensor).

Legacy path (still works): enrol with Adafruit's `enroll` example, then register the slot by hand:
```bash
curl -X POST localhost:8080/api/auth/enroll -H "X-Admin-Token: $VOTING_ADMIN_TOKEN" \
     -d sensor_id=1 --data-urlencode "name=Alice Johnson"
```
Those voters have no stored template.

### Firmware
`firmware/VotingTerminal/VotingTerminal.ino` (Arduino IDE; libraries: *Adafruit Fingerprint Sensor Library*, SoftwareSerial).
Edit the PIN block at the top. The voting half was rebuilt from the SCAN/VOTE protocol below; if you have your own working
sketch, paste in only the block between the two `ENROLMENT` banners and call `pollHost()` from `loop()`.
After compiling check the IDE's "dynamic memory" line: the 512-byte template buffer must leave a few hundred bytes free.

### Enrolment API
| Method & path | Auth | Purpose |
|---|---|---|
| POST `/api/enroll/start` `voter_ref,name,finger` | none (localhost) | start; 202 or 409 busy / already enrolled / voting started, 503 terminal offline |
| GET `/api/enroll/status` | none | live state: `starting capture1 lift capture2 processing downloading saving done failed` |
| POST `/api/enroll/cancel` | none | abort and clean the sensor slot |
| GET `/api/enroll/voter?voter_ref=` | none | is this voter enrolled (metadata only) |
| GET `/api/enroll/registry` | admin | all enrolments, metadata only |
| POST `/api/enroll/delete` `voter_ref` | admin | remove DB rows + sensor slot (refused once the voter has voted) |

Template bytes are never returned by any endpoint.

## REST API (admin = header `X-Admin-Token`)
| Method & path | Auth | Structure | Purpose |
|---|---|---|---|
| GET `/api/auth/session` | public | | booth state, hardware connected |
| GET `/api/auth/search?type=candidate\|voter&q=&limit=` | candidate public, voter admin | Trie O(L) | autocomplete |
| POST `/api/auth/enroll` `sensor_id,name` | admin | hash table + Trie | register voter |
| POST `/api/auth/candidates` `name` | admin | Trie + MaxHeap | add candidate |
| GET `/api/vote/candidates` | public | | ballot list |
| GET `/api/vote/audit?from=&to=&limit=` | admin | SkipList range | in-memory audit ledger |
| GET `/api/vote/verify` | admin | | re-hash the whole on-disk chain |
| GET `/api/results/leaderboard?k=` | admin (or public with `VOTING_PUBLIC_RESULTS=1`) | MaxHeap | live tallies, no SQL |
| GET `/api/results/turnout?from=&to=` (ms) | admin | SegmentTree O(log n) | votes in a time range |
| GET `/api/results/timeline?from=&to=&step_min=` | admin | SegmentTree | turnout series |

### Portal voting (the live finger is checked against the template stored in SQLite)
| Method & path | Auth | Purpose |
|---|---|---|
| POST `/api/vote/biometric/start` `voter_ref` | none (localhost) | loads the voter's enrolled template **from the `biometric_templates` table** (SHA-256 re-checked), sends it to the UNO (`VERIFY:<bytes>`, `D:<n>:<hex>`), the UNO captures a live finger and the R307S runs its 1:1 `Match` against that database template |
| GET `/api/vote/biometric/status` | none | `waiting matched failed idle` + message, match score, session timer |
| POST `/api/vote/biometric/cancel` | none | abort the capture |
| POST `/api/vote/cast` `voter_ref,candidate` | none | accepted only inside a session opened by a successful match for **that voter**; single use |

No finger gives `VF:ERR:TIMEOUT`, a different finger `NOMATCH`, a weak score `LOWSCORE`; none of these opens a session. If the
terminal still runs old firmware it never answers `VERIFY` and the portal reports it after 8 s instead of letting anyone through.
The flash search is not used for the portal at all: the reference is always the database copy.
`POST /api/auth/biometric/scan` (client-supplied sensor id, used for testing) now requires the admin token.
The physical terminal path (`SCAN:` / `VOTE:` buttons) is unchanged. Re-flash the firmware after pulling this change.

## SQL / DBMS layer (new in v2)
Schema v2: `constituencies`, `parties`, `voters`, `candidates`, `votes`, `audit_log`, `meta` with PK/FK/UNIQUE/CHECK,
a partial index, 11 triggers (append-only ledger, in-database hash-chain guard, trigger-fed audit log) and 5 views
(window functions, CTE, LAG). See `docs/`. **If you ran v1, delete `database/voting.db`** (the server refuses old files).

| Method & path (admin) | Purpose |
|---|---|
| GET `/api/sql/reports` / `/api/sql/report/<name>` | whitelisted SQL views: leaderboard, party-tally, constituency-turnout, hourly-turnout, remaining-voters, broken-chain-links, audit-log |
| GET `/api/sql/explain/<name>` | `EXPLAIN QUERY PLAN` of that report |
| GET `/api/sql/consistency` | MaxHeap vs SQL GROUP BY and SegmentTree vs SQL COUNT: equality check plus timings |
| POST `/api/auth/parties`, `/api/auth/constituencies` | reference data; `enroll` takes `constituency_id`, `candidates` takes `party_id` |

Demo data: `./build/voting_backend --db ../database/voting.db --simulate 400 --seed-only` (refuses a non-empty DB).
Then run the two scripts in `database/sql/` with the `sqlite3` shell.

## Protocol (newline-terminated, 9600 baud)
`SCAN:<id>` -> `ALLOW` (session opens, 35 s, single use) or `DENY`.
`VOTE:<NAME>` -> `ALLOW` = recorded, `DENY` = rejected. Compatible with the earlier Arduino sketch.

Enrolment (additive, lines <= 63 bytes): host `ENROLL:<slot>` / `DEL:<slot>` / `CANCEL`; terminal `EN:READY:<slot>`,
`EN:PLACE:1`, `EN:LIFT`, `EN:PLACE:2`, `EN:PROC`, `EN:STORED`, `T:<n>:<hex>`..., `T:END:<bytes>:<crc16>`,
`EN:ERR:<TIMEOUT|IMAGE|MISMATCH|DUPLICATE|STORE|DOWNLOAD|SENSOR|SLOT_USED|BUSY>`, `EN:DELETED`.

## Design notes
* **Vote path:** scan -> O(1) hash-table check -> session -> `chainMu_` { seq++ ; SkipList audit entry (Pending) ;
  one SQLite transaction (head check + `has_voted 0->1` + ledger row) ; Committed ; head advance ; heap + segment tree }.
  If the commit fails the audit entry is marked `failed` and the chain head does not move.
* **Hash chain:** `SHA256(candidate | timestamp_ms | previous_hash)`; genesis = 64 zeros. Triggers make `votes`
  append-only and `has_voted` irreversible; `/api/vote/verify` catches anyone who bypasses them.
* **SQL is the source of truth; structures are caches.** Heap/segment tree/skip list are rebuilt from `votes` at startup and cross-checked against SQL by `/api/sql/consistency`.
* **Complexity honesty:** MaxHeap `top()` is O(1) and updates O(log n); `topK` copies the (tiny) heap, O(n + k log n).
  SQLite `INTEGER PRIMARY KEY` *is* the B-tree on `sensor_id`. SkipList readers are lock-free, writers take one short mutex
  (appends are inherently ordered by the hash chain anyway). Turnout is bucket-granular (default 1 min).
* **Ballot secrecy:** `votes` has no voter column and logs never pair voter and candidate.
  Timestamps can still correlate with the scan time; do not retain scan logs.

## Limits (read before real elections)
* Prototype-grade for the serial link: it is unauthenticated, so anyone with USB/serial access can inject `SCAN:` lines.
  Add HMAC-signed frames with a shared key for anything serious.
* A hash chain detects edits but cannot stop someone with DB write access from recomputing the *entire* chain.
  Record the head hash (shown at shutdown / `/api/vote/verify`) somewhere external.
* No TLS and no user accounts: bound to localhost with one admin token. Put a reverse proxy in front if you expose it.
* Fingerprint matching happens on the R307S, not here; the backend trusts the template ID it is sent.
* `/api/enroll/start` is unauthenticated because the portal has no server-side voter accounts yet (the voter login lives in
  the browser's localStorage). Server-side guards limit the damage (one enrolment per voter_ref, one at a time, closed after
  the first ballot, localhost only), but anyone at the PC can enrol under any Voter ID. Real fix: authenticate voters on the server.
* The sensor's own slot table must be the one the database describes: do not enrol on the sensor with other tools after
  switching to this flow (orphan slots are skipped automatically, never overwritten).
* The firmware and the sensor read-back are **untested on real hardware** here (the protocol, CRC, database and UI were
  tested against a simulated terminal on a pty: `python3 backend/tests/e2e_enroll.py backend/build/voting_backend`).
* Tested on Linux (unit tests incl. ThreadSanitizer on the skip list, plus pty-based end-to-end and restart tests).
  The Windows `COM` path in `SerialListener.cpp` is written but **not compiled or run here**.
