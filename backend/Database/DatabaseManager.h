#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../Models/Candidate.h"
#include "../Models/Vote.h"
#include "../Models/Voter.h"

struct sqlite3;  // global C type; <sqlite3.h> is only included in the .cpp

namespace bvs {

class DatabaseManager {
public:
    struct CommitResult {
        bool ok = false;
        int64_t voteId = 0;
        std::string error;
    };
    struct ChainReport {
        bool ok = true;
        int64_t checked = 0;
        int64_t badId = 0;   // first row whose link/hash is wrong (0 if ok)
        std::string head;    // hash of last verified row (genesis if empty)
    };

    struct BiometricInfo {
        int sensorId = 0;
        std::string voterRef, name, finger, sha256;
        int templateBytes = 0;
        int64_t enrolledAtMs = 0;
        bool hasVoted = false;
    };

    explicit DatabaseManager(const std::string& path);  // throws std::runtime_error
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    void applySchema(const std::string& schemaFile);

    std::vector<Voter> loadVoters();
    std::vector<Candidate> loadCandidates();
    std::vector<Vote> loadVotes();

    bool addVoter(int sensorId, const std::string& name, int constituencyId = 0);  // 0 -> NULL
    int addCandidate(const std::string& name, int partyId = 0, const std::string& symbol = "🗳️", int constituencyId = 0);
    int addParty(const std::string& name, const std::string& symbol = "🚩");
    int addConstituency(const std::string& name, const std::string& state = "Maharashtra");
    bool resetElection(std::string* error = nullptr); // Clears ballots & resets has_voted; preserves fingerprints & voters

    // ---- fingerprint enrolment ------------------------------------------------------------
    bool enrollWithTemplate(int sensorId, const std::string& name, int constituencyId, const std::string& voterRef,
                            const std::string& finger, const std::string& templateBytes, const std::string& sha256,
                            std::string* error = nullptr);
    bool deleteEnrollment(int sensorId, std::string* error = nullptr);
    std::optional<BiometricInfo> biometricByRef(const std::string& voterRef);
    std::optional<BiometricInfo> biometricBySensor(int sensorId);
    std::optional<std::string> loadTemplate(int sensorId);
    int lowestFreeSlot(int lo, int hi);
    std::vector<BiometricInfo> listBiometrics();

    CommitResult commitVote(int sensorId, int candidateId, const std::string& candidateName, int64_t timestampMs,
                            const std::string& previousHash, const std::string& currentHash);

    std::string lastHash();
    ChainReport verifyChain();

    // ---- SQL reporting layer -------------------------------------------------------------
    std::string queryJson(const std::string& sql, const std::vector<int64_t>& params = {}, size_t maxRows = 1000);
    int64_t scalar(const std::string& sql, const std::vector<int64_t>& params = {});
    std::vector<std::pair<int, int64_t>> sqlTallies();
    int64_t sqlVotesBetween(int64_t fromMs, int64_t toMs);

    std::optional<std::string> getMeta(const std::string& key);
    void setMeta(const std::string& key, const std::string& value);

private:
    void exec(const char* sql);
    sqlite3* db_ = nullptr;
    std::mutex mu_;
};

}  // namespace bvs
