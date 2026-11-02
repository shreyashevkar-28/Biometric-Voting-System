#pragma once
#include <memory>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../DataStructures/Trie.h"
#include "../Database/DatabaseManager.h"
#include "../Models/Candidate.h"

namespace bvs {

// Identity + eligibility. Holds:
//   * voter hash table  (sensor_id -> has_voted)   O(1) authorization, no disk I/O
//   * Trie of voter names and Trie of candidate names   O(L) autocomplete
class AuthService {
public:
    enum class ScanResult { Allowed, NotEnrolled, AlreadyVoted };

    AuthService(DatabaseManager& db, std::string adminToken);

    void load();  // from DB at startup

    ScanResult checkScan(int sensorId) const;
    void markVoted(int sensorId);  // call only after the DB commit succeeded

    bool enrollVoter(int sensorId, const std::string& name, int constituencyId = 0);
    // Fingerprint enrolment: voter row + captured template in ONE DB transaction, then the caches.
    bool enrollWithTemplate(int sensorId, const std::string& name, int constituencyId, const std::string& voterRef,
                            const std::string& finger, const std::string& templateBytes, const std::string& sha256,
                            std::string* error = nullptr);
    bool removeEnrollment(int sensorId, std::string* error = nullptr);  // false if already voted / unknown
    int addCandidate(const std::string& name, int partyId = 0);  // new id, -1 if invalid/duplicate
    int addParty(const std::string& name);
    int addConstituency(const std::string& name);

    std::vector<std::pair<std::string, int>> searchVoters(const std::string& prefix, size_t limit) const;
    std::vector<std::pair<std::string, int>> searchCandidates(const std::string& prefix, size_t limit) const;

    std::vector<Candidate> candidates() const;
    std::optional<Candidate> candidateByKey(const std::string& normalizedKey) const;
    size_t voterCount() const;

    bool isAdmin(const std::string& token) const;

    static bool validCandidateName(const std::string& n);
    static bool validVoterName(const std::string& n);

private:
    DatabaseManager& db_;
    const std::string adminToken_;
    mutable std::shared_mutex mu_;
    std::unordered_map<int, bool> voters_;                 // sensor_id -> has_voted
    std::unordered_map<std::string, Candidate> byKey_;     // normalizeKey(name) -> candidate
    std::vector<Candidate> candidateList_;
    std::unique_ptr<Trie> voterTrie_ = std::make_unique<Trie>();      // rebuilt on removal (Trie has no erase)
    std::unique_ptr<Trie> candidateTrie_ = std::make_unique<Trie>();
};

}  // namespace bvs
