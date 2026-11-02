#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../DataStructures/SkipList.h"
#include "../Database/DatabaseManager.h"
#include "AuthService.h"
#include "ResultService.h"

namespace bvs {

// Voting state machine + tamper-evident ledger.
//
//   SCAN:<id>  -> AuthService (hash table) -> ALLOW opens a single-use voting session / DENY
//   VERIFY:<bytes> (web portal) -> the voter's enrolled template is read from SQLite and streamed to the terminal
//        (D:<n>:<hex> ... D:END:<crc>); the terminal captures a REAL finger, loads the DATABASE template into the
//        sensor and runs the sensor's 1:1 Match. Replies VF:READY | VF:D:<n> | VF:PLACE | VF:OK:<score> | VF:ERR:<CODE>.
//        Only a match against THAT voter's database template opens the session.
//   VOTE:<c>   -> only valid inside a session ->
//        [chainMu_] seq++ -> SkipList audit insert (Pending) -> SQLite txn (flag + hash-chained row)
//                  -> SkipList status Committed -> heap + segment tree update -> ALLOW (confirm)
class VotingService {
public:
    struct SessionInfo {
        bool active = false;
        int64_t remainingMs = 0;
    };
    struct AuditView {
        uint64_t seq;
        std::string candidate;
        int64_t timestampMs;
        std::string previousHash;
        std::string hash;
        std::string status;  // pending | committed | failed
    };

    using Sender = std::function<bool(const std::string&)>;
    struct VerifyStart {
        bool ok = false;
        int http = 200;
        std::string error;
    };

    VotingService(DatabaseManager& db, AuthService& auth, ResultService& results, int64_t sessionTimeoutMs = 35000);

    // Wire the serial link (used by the web-portal fingerprint verification).
    void setTerminal(Sender send, std::function<bool()> connected);

    // Web-portal verification: asks the sensor for a live capture + 1:N search and requires the hit to be the
    // voter's own slot. Nothing opens a voting session except a real sensor match.
    VerifyStart startVerify(const std::string& voterRef, int expectedSlot);
    void cancelVerify();
    std::string verifyStatusJson();   // applies the timeout, then serialises the current state

    // Ballot submitted from the portal. Succeeds only inside a live session that belongs to `sensorId`.
    // Returns ALLOW | DENY | NOSESSION | UNKNOWN_CANDIDATE | BAD_CANDIDATE.
    std::string castFor(int sensorId, const std::string& candidateName);

    void init();  // rehydrate audit skip list + results + chain head from disk

    // Serial protocol entry point: returns the reply line ("ALLOW"/"DENY") or "" for none.
    std::string handleLine(const std::string& line);

    SessionInfo session() const;
    std::vector<AuditView> audit(uint64_t from, uint64_t to, size_t limit) const;
    size_t auditSize() const { return audit_.size(); }
    DatabaseManager::ChainReport verifyChain() { return db_.verifyChain(); }
    std::string headHash() const;

private:
    enum class VState { Idle, Waiting, Sending, Capturing, Matched, Failed };

    std::string onScan(int sensorId);
    std::string onVote(const std::string& candidateKey);
    std::string onVerifyLine(const std::string& ev);
    void failVerify(const std::string& code);   // caller holds verifyMu_
    void expireVerifyIfNeeded();                // caller holds verifyMu_
    void sendChunk();                           // caller holds verifyMu_
    bool sessionLive() const;
    bool commitVote(int sensorId, const Candidate& cand);
    void clearSession();

    DatabaseManager& db_;
    AuthService& auth_;
    ResultService& results_;
    const int64_t sessionTimeoutMs_;

    SkipList audit_;

    mutable std::mutex chainMu_;  // serializes chain head advance (inherently sequential)
    std::string headHash_ = kGenesisHashStr();
    uint64_t seq_ = 0;
    int64_t lastTs_ = 0;

    mutable std::mutex sessionMu_;
    int pendingSensor_ = -1;
    int64_t deadlineMs_ = 0;

    // web-portal verification state (lock order: verifyMu_ -> sessionMu_ -> chainMu_)
    mutable std::mutex verifyMu_;
    Sender send_;
    std::function<bool()> connected_;
    VState vstate_ = VState::Idle;
    std::string vRef_, vMsg_, vCode_;
    int vSlot_ = -1;
    int vConfidence_ = 0;
    int64_t vDeadlineMs_ = 0;
    std::string vTpl_, vSha_;   // enrolled template read from SQLite for the signed-in voter
    size_t vOffset_ = 0;
    int vNext_ = 0;

    static std::string kGenesisHashStr();
};

}  // namespace bvs
