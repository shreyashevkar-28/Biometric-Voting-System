#include "AuthService.h"

#include "../Common/Util.h"

namespace bvs {

AuthService::AuthService(DatabaseManager& db, std::string adminToken) : db_(db), adminToken_(std::move(adminToken)) {}

bool AuthService::validCandidateName(const std::string& n) {
    if (n.empty() || n.size() > 20) return false;
    for (unsigned char c : n)
        if (!(std::isalnum(c) || c == '_' || c == ' ')) return false;  // must survive the serial protocol
    return true;
}

bool AuthService::validVoterName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

void AuthService::load() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    voters_.clear();
    byKey_.clear();
    candidateList_.clear();
    voterTrie_ = std::make_unique<Trie>();
    candidateTrie_ = std::make_unique<Trie>();
    for (const auto& v : db_.loadVoters()) {
        voters_[v.sensorId] = v.hasVoted;
        voterTrie_->insert(v.name, v.sensorId);
    }
    for (const auto& c : db_.loadCandidates()) {
        byKey_[normalizeKey(c.name)] = c;
        candidateList_.push_back(c);
        candidateTrie_->insert(c.name, c.id);
    }
}

AuthService::ScanResult AuthService::checkScan(int sensorId) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = voters_.find(sensorId);  // O(1)
    if (it == voters_.end()) return ScanResult::NotEnrolled;
    return it->second ? ScanResult::AlreadyVoted : ScanResult::Allowed;
}

void AuthService::markVoted(int sensorId) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto it = voters_.find(sensorId);
    if (it != voters_.end()) it->second = true;
}

bool AuthService::enrollVoter(int sensorId, const std::string& name, int constituencyId) {
    if (sensorId < 0 || sensorId > 65535 || !validVoterName(name)) return false;
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (voters_.count(sensorId)) return false;
    if (!db_.addVoter(sensorId, name, constituencyId)) return false;
    voters_[sensorId] = false;
    voterTrie_->insert(name, sensorId);
    return true;
}

bool AuthService::enrollWithTemplate(int sensorId, const std::string& name, int constituencyId,
                                     const std::string& voterRef, const std::string& finger,
                                     const std::string& templateBytes, const std::string& sha256, std::string* error) {
    if (sensorId < 0 || sensorId > 65535 || !validVoterName(name)) {
        if (error) *error = "invalid sensor slot or name";
        return false;
    }
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (voters_.count(sensorId)) {
        if (error) *error = "sensor slot already registered";
        return false;
    }
    if (!db_.enrollWithTemplate(sensorId, name, constituencyId, voterRef, finger, templateBytes, sha256, error))
        return false;
    voters_[sensorId] = false;
    voterTrie_->insert(name, sensorId);
    return true;
}

bool AuthService::removeEnrollment(int sensorId, std::string* error) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!voters_.count(sensorId)) {
        if (error) *error = "unknown voter";
        return false;
    }
    if (!db_.deleteEnrollment(sensorId, error)) return false;   // trigger blocks voters who already voted
    voters_.erase(sensorId);
    voterTrie_ = std::make_unique<Trie>();
    for (const auto& v : db_.loadVoters()) voterTrie_->insert(v.name, v.sensorId);
    return true;
}

int AuthService::addCandidate(const std::string& name, int partyId) {
    if (!validCandidateName(name)) return -1;
    std::unique_lock<std::shared_mutex> lk(mu_);
    const std::string key = normalizeKey(name);
    if (byKey_.count(key)) return -1;
    int id = db_.addCandidate(name, partyId);
    if (id < 0) return -1;
    Candidate c{id, name, partyId};
    byKey_[key] = c;
    candidateList_.push_back(c);
    candidateTrie_->insert(name, id);
    return id;
}

int AuthService::addParty(const std::string& name) {
    if (!validVoterName(name)) return -1;
    std::unique_lock<std::shared_mutex> lk(mu_);
    return db_.addParty(name);
}

int AuthService::addConstituency(const std::string& name) {
    if (!validVoterName(name)) return -1;
    std::unique_lock<std::shared_mutex> lk(mu_);
    return db_.addConstituency(name);
}

std::vector<std::pair<std::string, int>> AuthService::searchVoters(const std::string& prefix, size_t limit) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return voterTrie_->autocomplete(prefix, limit);
}

std::vector<std::pair<std::string, int>> AuthService::searchCandidates(const std::string& prefix, size_t limit) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return candidateTrie_->autocomplete(prefix, limit);
}

std::vector<Candidate> AuthService::candidates() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return candidateList_;
}

std::optional<Candidate> AuthService::candidateByKey(const std::string& key) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    auto it = byKey_.find(key);
    if (it == byKey_.end()) return std::nullopt;
    return it->second;
}

size_t AuthService::voterCount() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return voters_.size();
}

bool AuthService::isAdmin(const std::string& token) const {
    return !adminToken_.empty() && constantTimeEquals(token, adminToken_);
}

}  // namespace bvs
