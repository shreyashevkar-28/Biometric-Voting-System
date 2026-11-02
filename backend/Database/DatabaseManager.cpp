#include "DatabaseManager.h"

#include <sqlite3.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "../Common/Crypto.h"
#include "../Common/Util.h"

namespace bvs {
namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

std::string colText(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? reinterpret_cast<const char*>(t) : std::string();
}

void bindIntOrNull(sqlite3_stmt* s, int i, int v) {
    if (v > 0) sqlite3_bind_int(s, i, v);
    else sqlite3_bind_null(s, i);
}

void bindText(sqlite3_stmt* s, int i, const std::string& v) {
    sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

}  // namespace

DatabaseManager::DatabaseManager(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open database '" + path + "': " + err);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=FULL;");   // a vote is durable before it is confirmed
    exec("PRAGMA foreign_keys=ON;");
}

DatabaseManager::~DatabaseManager() {
    if (db_) sqlite3_close(db_);
}

void DatabaseManager::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec failed: " + m);
    }
}

void DatabaseManager::applySchema(const std::string& schemaFile) {
    std::ifstream in(schemaFile, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read schema file: " + schemaFile);
    std::stringstream ss;
    ss << in.rdbuf();
    std::lock_guard<std::mutex> lk(mu_);
    {   // in-place upgrade of a v2 database created before fingerprint enrolment existed
        Stmt tbl(db_, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='voters'");
        Stmt col(db_, "SELECT COUNT(*) FROM pragma_table_info('voters') WHERE name='voter_ref'");
        sqlite3_step(tbl.s);
        sqlite3_step(col.s);
        if (sqlite3_column_int(tbl.s, 0) > 0 && sqlite3_column_int(col.s, 0) == 0)
            exec("ALTER TABLE voters ADD COLUMN voter_ref TEXT CHECK (voter_ref IS NULL OR length(voter_ref) BETWEEN 1 AND 32)");
    }
    {   // refuse to run on a database created by the v1 schema (votes without candidate_id)
        Stmt any(db_, "SELECT COUNT(*) FROM pragma_table_info('votes')");
        Stmt has(db_, "SELECT COUNT(*) FROM pragma_table_info('votes') WHERE name='candidate_id'");
        sqlite3_step(any.s);
        sqlite3_step(has.s);
        if (sqlite3_column_int(any.s, 0) > 0 && sqlite3_column_int(has.s, 0) == 0)
            throw std::runtime_error("this database uses the old v1 schema. Delete it (or use --db <new file>) and start again.");
    }
    exec(ss.str().c_str());
    exec("INSERT INTO meta(key, value) VALUES('schema_version', '2') ON CONFLICT(key) DO UPDATE SET value=excluded.value");
}

std::vector<Voter> DatabaseManager::loadVoters() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT sensor_id, name, has_voted, COALESCE(constituency_id, 0) FROM voters ORDER BY sensor_id");
    std::vector<Voter> out;
    while (sqlite3_step(s.s) == SQLITE_ROW)
        out.push_back(Voter{sqlite3_column_int(s.s, 0), colText(s.s, 1), sqlite3_column_int(s.s, 2) != 0, sqlite3_column_int(s.s, 3)});
    return out;
}

std::vector<Candidate> DatabaseManager::loadCandidates() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT c.id, c.name, COALESCE(c.party_id, 0), COALESCE(c.symbol, '🗳️'), COALESCE(p.name, 'Independent'), COALESCE(c.constituency_id, 0) FROM candidates c LEFT JOIN parties p ON p.party_id = c.party_id ORDER BY c.id");
    std::vector<Candidate> out;
    while (sqlite3_step(s.s) == SQLITE_ROW)
        out.push_back(Candidate{sqlite3_column_int(s.s, 0), colText(s.s, 1), sqlite3_column_int(s.s, 2),
                                colText(s.s, 3), colText(s.s, 4), sqlite3_column_int(s.s, 5)});
    return out;
}

std::vector<Vote> DatabaseManager::loadVotes() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT id, candidate_name, timestamp_ms, previous_hash, current_hash FROM votes ORDER BY id");
    std::vector<Vote> out;
    while (sqlite3_step(s.s) == SQLITE_ROW)
        out.push_back(Vote{sqlite3_column_int64(s.s, 0), colText(s.s, 1), sqlite3_column_int64(s.s, 2),
                           colText(s.s, 3), colText(s.s, 4)});
    return out;
}

bool DatabaseManager::addVoter(int sensorId, const std::string& name, int constituencyId) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO voters(sensor_id, name, constituency_id, has_voted) VALUES(?1, ?2, ?3, 0)");
    sqlite3_bind_int(s.s, 1, sensorId);
    bindText(s.s, 2, name);
    bindIntOrNull(s.s, 3, constituencyId);
    return sqlite3_step(s.s) == SQLITE_DONE;
}

int DatabaseManager::addCandidate(const std::string& name, int partyId, const std::string& symbol, int constituencyId) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO candidates(name, party_id, symbol, constituency_id) VALUES(?1, ?2, ?3, ?4)");
    bindText(s.s, 1, name);
    bindIntOrNull(s.s, 2, partyId);
    bindText(s.s, 3, symbol.empty() ? "🗳️" : symbol);
    bindIntOrNull(s.s, 4, constituencyId);
    if (sqlite3_step(s.s) != SQLITE_DONE) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

int DatabaseManager::addParty(const std::string& name, const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO parties(name, symbol) VALUES(?1, ?2)");
    bindText(s.s, 1, name);
    bindText(s.s, 2, symbol.empty() ? "🚩" : symbol);
    if (sqlite3_step(s.s) != SQLITE_DONE) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

int DatabaseManager::addConstituency(const std::string& name, const std::string& state) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO constituencies(name, state) VALUES(?1, ?2)");
    bindText(s.s, 1, name);
    bindText(s.s, 2, state.empty() ? "Maharashtra" : state);
    if (sqlite3_step(s.s) != SQLITE_DONE) return -1;
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

bool DatabaseManager::resetElection(std::string* error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto fail = [&](const std::string& m) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        if (error) *error = m;
        return false;
    };
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(std::string("begin failed: ") + sqlite3_errmsg(db_));

    sqlite3_exec(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES('allow_election_reset', '1')", nullptr, nullptr, nullptr);
    if (sqlite3_exec(db_, "DELETE FROM votes", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(std::string("failed to clear votes: ") + sqlite3_errmsg(db_));
    if (sqlite3_exec(db_, "UPDATE voters SET has_voted = 0", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(std::string("failed to reset voters has_voted: ") + sqlite3_errmsg(db_));
    sqlite3_exec(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES('allow_election_reset', '0')", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "INSERT INTO audit_log(event_type, detail) VALUES('ELECTION_RESET', 'Polls reset by administrator. Biometric templates & voters preserved.')", nullptr, nullptr, nullptr);

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(sqlite3_errmsg(db_));
    return true;
}

bool DatabaseManager::enrollWithTemplate(int sensorId, const std::string& name, int constituencyId,
                                         const std::string& voterRef, const std::string& finger,
                                         const std::string& templateBytes, const std::string& sha256,
                                         std::string* error) {
    std::lock_guard<std::mutex> lk(mu_);
    auto fail = [&](const std::string& m) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        if (error) *error = m;
        return false;
    };
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return fail(std::string("begin failed: ") + sqlite3_errmsg(db_));
    try {
        {
            Stmt s(db_, "INSERT INTO voters(sensor_id, name, constituency_id, has_voted, voter_ref) VALUES(?1,?2,?3,0,?4)");
            sqlite3_bind_int(s.s, 1, sensorId);
            bindText(s.s, 2, name);
            bindIntOrNull(s.s, 3, constituencyId);
            if (voterRef.empty()) sqlite3_bind_null(s.s, 4); else bindText(s.s, 4, voterRef);
            if (sqlite3_step(s.s) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
        }
        {
            Stmt s(db_, "INSERT INTO biometric_templates(sensor_id, template, template_sha256, template_bytes, finger) "
                        "VALUES(?1,?2,?3,?4,?5)");
            sqlite3_bind_int(s.s, 1, sensorId);
            sqlite3_bind_blob(s.s, 2, templateBytes.data(), static_cast<int>(templateBytes.size()), SQLITE_TRANSIENT);
            bindText(s.s, 3, sha256);
            sqlite3_bind_int(s.s, 4, static_cast<int>(templateBytes.size()));
            bindText(s.s, 5, finger);
            if (sqlite3_step(s.s) != SQLITE_DONE) return fail(sqlite3_errmsg(db_));
        }
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(db_));
        return true;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

bool DatabaseManager::deleteEnrollment(int sensorId, std::string* error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "DELETE FROM voters WHERE sensor_id=?1");
    sqlite3_bind_int(s.s, 1, sensorId);
    if (sqlite3_step(s.s) != SQLITE_DONE) {
        if (error) *error = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) == 1;
}

namespace {
DatabaseManager::BiometricInfo readInfo(sqlite3_stmt* s) {
    DatabaseManager::BiometricInfo b;
    b.sensorId = sqlite3_column_int(s, 0);
    b.voterRef = colText(s, 1);
    b.name = colText(s, 2);
    b.hasVoted = sqlite3_column_int(s, 3) != 0;
    b.finger = colText(s, 4);
    b.sha256 = colText(s, 5);
    b.templateBytes = sqlite3_column_int(s, 6);
    b.enrolledAtMs = sqlite3_column_int64(s, 7);
    return b;
}
const char* kInfoSql =
    "SELECT v.sensor_id, COALESCE(v.voter_ref,''), v.name, v.has_voted, b.finger, b.template_sha256, "
    "b.template_bytes, b.enrolled_at_ms FROM voters v JOIN biometric_templates b ON b.sensor_id = v.sensor_id ";
}  // namespace

std::optional<DatabaseManager::BiometricInfo> DatabaseManager::biometricByRef(const std::string& voterRef) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string(kInfoSql) + "WHERE v.voter_ref = ?1").c_str());
    bindText(s.s, 1, voterRef);
    if (sqlite3_step(s.s) == SQLITE_ROW) return readInfo(s.s);
    return std::nullopt;
}

std::optional<DatabaseManager::BiometricInfo> DatabaseManager::biometricBySensor(int sensorId) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string(kInfoSql) + "WHERE v.sensor_id = ?1").c_str());
    sqlite3_bind_int(s.s, 1, sensorId);
    if (sqlite3_step(s.s) == SQLITE_ROW) return readInfo(s.s);
    return std::nullopt;
}

std::optional<std::string> DatabaseManager::loadTemplate(int sensorId) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT template, template_sha256 FROM biometric_templates WHERE sensor_id = ?1");
    sqlite3_bind_int(s.s, 1, sensorId);
    if (sqlite3_step(s.s) != SQLITE_ROW) return std::nullopt;
    const void* blob = sqlite3_column_blob(s.s, 0);
    const int n = sqlite3_column_bytes(s.s, 0);
    if (!blob || n <= 0) return std::nullopt;
    std::string bytes(static_cast<const char*>(blob), static_cast<size_t>(n));
    if (sha256Hex(bytes) != colText(s.s, 1)) return std::nullopt;   // integrity check against the stored hash
    return bytes;
}

std::vector<DatabaseManager::BiometricInfo> DatabaseManager::listBiometrics() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string(kInfoSql) + "ORDER BY v.sensor_id").c_str());
    std::vector<BiometricInfo> out;
    while (sqlite3_step(s.s) == SQLITE_ROW) out.push_back(readInfo(s.s));
    return out;
}

int DatabaseManager::lowestFreeSlot(int lo, int hi) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT sensor_id FROM voters WHERE sensor_id BETWEEN ?1 AND ?2 ORDER BY sensor_id");
    sqlite3_bind_int(s.s, 1, lo);
    sqlite3_bind_int(s.s, 2, hi);
    int expect = lo;
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        const int id = sqlite3_column_int(s.s, 0);
        if (id != expect) break;
        ++expect;
    }
    return expect <= hi ? expect : -1;
}

DatabaseManager::CommitResult DatabaseManager::commitVote(int sensorId, int candidateId,
                                                          const std::string& candidateName, int64_t timestampMs, const std::string& previousHash,
                                                          const std::string& currentHash) {
    std::lock_guard<std::mutex> lk(mu_);
    auto rollback = [&] { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); };
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return {false, 0, std::string("begin failed: ") + sqlite3_errmsg(db_)};
    try {
        {  // 1) the chain head the caller used must match what is on disk
            Stmt s(db_, "SELECT current_hash FROM votes ORDER BY id DESC LIMIT 1");
            std::string head = kGenesisHash;
            if (sqlite3_step(s.s) == SQLITE_ROW) head = colText(s.s, 0);
            if (head != previousHash) {
                rollback();
                return {false, 0, "chain head mismatch"};
            }
        }
        {  // 2) one-person-one-vote enforced by the database itself
            Stmt s(db_, "UPDATE voters SET has_voted=1 WHERE sensor_id=?1 AND has_voted=0");
            sqlite3_bind_int(s.s, 1, sensorId);
            if (sqlite3_step(s.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
            if (sqlite3_changes(db_) != 1) {
                rollback();
                return {false, 0, "voter missing or already voted"};
            }
        }
        {  // 3) append to the hash-chained ledger
            Stmt s(db_, "INSERT INTO votes(candidate_id, candidate_name, timestamp_ms, previous_hash, current_hash) "
                        "VALUES(?1, ?2, ?3, ?4, ?5)");
            sqlite3_bind_int(s.s, 1, candidateId);
            bindText(s.s, 2, candidateName);
            sqlite3_bind_int64(s.s, 3, timestampMs);
            bindText(s.s, 4, previousHash);
            bindText(s.s, 5, currentHash);
            if (sqlite3_step(s.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
        }
        const int64_t id = sqlite3_last_insert_rowid(db_);
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
        return {true, id, ""};
    } catch (const std::exception& e) {
        rollback();
        return {false, 0, e.what()};
    }
}

std::string DatabaseManager::lastHash() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT current_hash FROM votes ORDER BY id DESC LIMIT 1");
    if (sqlite3_step(s.s) == SQLITE_ROW) return colText(s.s, 0);
    return kGenesisHash;
}

DatabaseManager::ChainReport DatabaseManager::verifyChain() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT id, candidate_name, timestamp_ms, previous_hash, current_hash FROM votes ORDER BY id");
    ChainReport rep;
    rep.head = kGenesisHash;
    while (sqlite3_step(s.s) == SQLITE_ROW) {
        const int64_t id = sqlite3_column_int64(s.s, 0);
        const std::string cand = colText(s.s, 1);
        const int64_t ts = sqlite3_column_int64(s.s, 2);
        const std::string prev = colText(s.s, 3);
        const std::string cur = colText(s.s, 4);
        if (prev != rep.head || chainHash(cand, ts, prev) != cur) {
            rep.ok = false;
            rep.badId = id;
            return rep;
        }
        rep.head = cur;
        ++rep.checked;
    }
    return rep;
}

std::string DatabaseManager::queryJson(const std::string& sql, const std::vector<int64_t>& params, size_t maxRows) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, sql.c_str());
    for (size_t i = 0; i < params.size(); ++i) sqlite3_bind_int64(s.s, static_cast<int>(i) + 1, params[i]);
    const int cols = sqlite3_column_count(s.s);
    std::string out = "[";
    size_t rows = 0;
    while (sqlite3_step(s.s) == SQLITE_ROW && rows < maxRows) {
        if (rows++) out += ",";
        out += "{";
        for (int c = 0; c < cols; ++c) {
            if (c) out += ",";
            out += jsonStr(sqlite3_column_name(s.s, c)) + ":";
            switch (sqlite3_column_type(s.s, c)) {
                case SQLITE_INTEGER: out += std::to_string(sqlite3_column_int64(s.s, c)); break;
                case SQLITE_FLOAT: {
                    char buf[48];
                    std::snprintf(buf, sizeof buf, "%.2f", sqlite3_column_double(s.s, c));
                    out += buf;
                    break;
                }
                case SQLITE_NULL: out += "null"; break;
                default: out += jsonStr(colText(s.s, c));
            }
        }
        out += "}";
    }
    return out + "]";
}

int64_t DatabaseManager::scalar(const std::string& sql, const std::vector<int64_t>& params) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, sql.c_str());
    for (size_t i = 0; i < params.size(); ++i) sqlite3_bind_int64(s.s, static_cast<int>(i) + 1, params[i]);
    return sqlite3_step(s.s) == SQLITE_ROW ? sqlite3_column_int64(s.s, 0) : 0;
}

std::vector<std::pair<int, int64_t>> DatabaseManager::sqlTallies() {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT c.id, COUNT(v.id) AS n FROM candidates c LEFT JOIN votes v ON v.candidate_id = c.id "
                "GROUP BY c.id ORDER BY n DESC, c.id ASC");
    std::vector<std::pair<int, int64_t>> out;
    while (sqlite3_step(s.s) == SQLITE_ROW) out.emplace_back(sqlite3_column_int(s.s, 0), sqlite3_column_int64(s.s, 1));
    return out;
}

int64_t DatabaseManager::sqlVotesBetween(int64_t fromMs, int64_t toMs) {
    return scalar("SELECT COUNT(*) FROM votes WHERE timestamp_ms >= ?1 AND timestamp_ms < ?2", {fromMs, toMs});
}

std::optional<std::string> DatabaseManager::getMeta(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT value FROM meta WHERE key=?1");
    bindText(s.s, 1, key);
    if (sqlite3_step(s.s) == SQLITE_ROW) return colText(s.s, 0);
    return std::nullopt;
}

void DatabaseManager::setMeta(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO meta(key, value) VALUES(?1, ?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    bindText(s.s, 1, key);
    bindText(s.s, 2, value);
    if (sqlite3_step(s.s) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_));
}

}  // namespace bvs
