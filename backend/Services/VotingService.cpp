#include "VotingService.h"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

#include <algorithm>
#include <cstdio>

#include "../Common/Crypto.h"
#include "EnrollmentService.h"
#include "../Common/Util.h"

namespace bvs {

std::string VotingService::kGenesisHashStr() { return kGenesisHash; }

VotingService::VotingService(DatabaseManager& db, AuthService& auth, ResultService& results, int64_t sessionTimeoutMs)
    : db_(db), auth_(auth), results_(results), sessionTimeoutMs_(sessionTimeoutMs) {}

void VotingService::init() {
    std::unordered_map<std::string, int> idByName;
    for (const auto& c : auth_.candidates()) idByName[normalizeKey(c.name)] = c.id;

    std::lock_guard<std::mutex> lk(chainMu_);
    for (const auto& v : db_.loadVotes()) {
        AuditRecord rec{static_cast<uint64_t>(v.id), v.candidateName, v.timestampMs, v.previousHash, v.currentHash};
        if (auto* node = audit_.insert(std::move(rec))) SkipList::setStatus(node, SkipList::Committed);
        auto it = idByName.find(normalizeKey(v.candidateName));
        if (it != idByName.end()) results_.recordVote(it->second, v.timestampMs);
        headHash_ = v.currentHash;
        seq_ = static_cast<uint64_t>(v.id);
        lastTs_ = v.timestampMs;
    }
}

std::string VotingService::headHash() const {
    std::lock_guard<std::mutex> lk(chainMu_);
    return headHash_;
}

// ---------------- protocol ----------------
std::string VotingService::handleLine(const std::string& line) {
    if (line.rfind("SCAN:", 0) == 0) {
        const std::string num = line.substr(5);
        if (num.empty() || num.size() > 5) return "DENY";
        for (unsigned char c : num)
            if (!std::isdigit(c)) return "DENY";
        return onScan(std::atoi(num.c_str()));
    }
    if (line.rfind("VF:", 0) == 0) return onVerifyLine(line.substr(3));
    if (line.rfind("VOTE:", 0) == 0) {
        const std::string name = line.substr(5);
        if (name.empty() || name.size() > 20) { clearSession(); return "DENY"; }
        for (unsigned char c : name)
            if (!(std::isalnum(c) || c == '_')) { clearSession(); return "DENY"; }
        return onVote(normalizeKey(name));
    }
    return "";  // unknown chatter is ignored
}

void VotingService::clearSession() {
    std::lock_guard<std::mutex> lk(sessionMu_);
    pendingSensor_ = -1;
    deadlineMs_ = 0;
}

VotingService::SessionInfo VotingService::session() const {
    std::lock_guard<std::mutex> lk(sessionMu_);
    SessionInfo s;
    const int64_t rem = deadlineMs_ - nowMs();
    if (pendingSensor_ >= 0 && rem > 0) {
        s.active = true;
        s.remainingMs = rem;
    }
    return s;
}

std::string VotingService::onScan(int sensorId) {
    clearSession();  // a new scan always supersedes an unfinished session
    if (auth_.checkScan(sensorId) != AuthService::ScanResult::Allowed) {
        std::cerr << "[vote] scan denied\n";
        return "DENY";
    }
    {
        std::lock_guard<std::mutex> lk(sessionMu_);
        pendingSensor_ = sensorId;
        deadlineMs_ = nowMs() + sessionTimeoutMs_;
    }
    return "ALLOW";
}

std::string VotingService::onVote(const std::string& candidateKey) {
    int sensor = -1;
    bool live = false;
    {
        std::lock_guard<std::mutex> lk(sessionMu_);
        sensor = pendingSensor_;
        live = pendingSensor_ >= 0 && nowMs() <= deadlineMs_;
        pendingSensor_ = -1;  // single-use, valid or not
        deadlineMs_ = 0;
    }
    if (!live) {
        std::cerr << "[vote] VOTE without a live session\n";
        return "DENY";
    }
    auto cand = auth_.candidateByKey(candidateKey);
    if (!cand) {
        std::cerr << "[vote] unknown candidate\n";
        return "DENY";
    }
    return commitVote(sensor, *cand) ? "ALLOW" : "DENY";
}


// ---------------- web-portal fingerprint verification ----------------
// Live finger (R307S capture) vs the template stored in SQLite for the signed-in voter.
namespace {
constexpr int64_t kStartAckMs = 8000;    // terminal must answer VERIFY quickly (old firmware never will)
constexpr int64_t kChunkMs = 8000;       // per template chunk acknowledgement
constexpr int64_t kCaptureMs = 45000;    // terminal waits 30 s for a finger, then matches
constexpr int kChunkBytes = 24;          // 24 bytes -> 48 hex chars; keeps lines < 64 on both ends
constexpr int kMinScore = 30;            // sensor reports 0 = no similarity; a genuine match is far above this

const char* verifyMessage(const std::string& code) {
    if (code == "TIMEOUT") return "No finger detected in time. Please try again.";
    if (code == "IMAGE") return "Could not read the fingerprint image. Clean the sensor and try again.";
    if (code == "NOMATCH") return "Fingerprint does not match the enrolled template. Please try again.";
    if (code == "LOWSCORE") return "Fingerprint match was too weak. Please try again.";
    if (code == "INELIGIBLE") return "This voter is not eligible to vote (not enrolled or already voted).";
    if (code == "SENSOR") return "Fingerprint sensor not responding. Check the wiring.";
    if (code == "DOWNLOAD") return "Could not load the enrolled template into the sensor.";
    if (code == "BUSY") return "The terminal is busy. Try again in a moment.";
    if (code == "NOFIRMWARE") return "The terminal did not answer. Re-flash firmware/VotingTerminal/VotingTerminal.ino and retry.";
    if (code == "CANCELLED") return "Verification cancelled.";
    return "Fingerprint verification failed.";
}

std::string hexEncode(const std::string& b) {
    static const char* d = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : b) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
    return o;
}
}  // namespace

void VotingService::setTerminal(Sender send, std::function<bool()> connected) {
    std::lock_guard<std::mutex> lk(verifyMu_);
    send_ = std::move(send);
    connected_ = std::move(connected);
}

bool VotingService::sessionLive() const {
    std::lock_guard<std::mutex> lk(sessionMu_);
    return pendingSensor_ >= 0 && nowMs() <= deadlineMs_;
}

void VotingService::failVerify(const std::string& code) {   // caller holds verifyMu_
    vstate_ = VState::Failed;
    vCode_ = code;
    vMsg_ = verifyMessage(code);
    vTpl_.clear();
    std::cerr << "[verify] failed: " << code << "\n";
}

void VotingService::expireVerifyIfNeeded() {   // caller holds verifyMu_
    const bool active = vstate_ == VState::Waiting || vstate_ == VState::Sending || vstate_ == VState::Capturing;
    if (active && nowMs() > vDeadlineMs_) {
        if (send_) send_("CANCEL");
        failVerify(vstate_ == VState::Waiting ? "NOFIRMWARE" : "TIMEOUT");
    }
}

VotingService::VerifyStart VotingService::startVerify(const std::string& voterRef, int expectedSlot) {
    VerifyStart r;
    auto bad = [&](int http, const char* msg) { r.http = http; r.error = msg; return r; };

    std::lock_guard<std::mutex> lk(verifyMu_);
    expireVerifyIfNeeded();
    if (vstate_ == VState::Waiting || vstate_ == VState::Sending || vstate_ == VState::Capturing)
        return bad(409, "another fingerprint verification is in progress");
    if (!send_ || !connected_ || !connected_()) return bad(503, "fingerprint terminal is not connected");
    if (auth_.checkScan(expectedSlot) != AuthService::ScanResult::Allowed)
        return bad(403, "voter is not enrolled or has already voted");

    // The reference template comes from the DATABASE, never from the sensor's own flash.
    auto tpl = db_.loadTemplate(expectedSlot);
    if (!tpl || tpl->size() < 64 || tpl->size() > 1024)
        return bad(404, "no valid enrolled template in the database for this voter");

    clearSession();   // a new verification always supersedes an earlier, unfinished session
    vRef_ = voterRef;
    vSlot_ = expectedSlot;
    vConfidence_ = 0;
    vCode_.clear();
    vTpl_ = std::move(*tpl);
    vSha_ = sha256Hex(vTpl_);
    vOffset_ = 0;
    vNext_ = 0;
    vstate_ = VState::Waiting;
    vMsg_ = "Loading your enrolled fingerprint from the database...";
    vDeadlineMs_ = nowMs() + kStartAckMs;

    if (!send_("VERIFY:" + std::to_string(vTpl_.size()))) {
        failVerify("SENSOR");
        return bad(503, "could not write to the terminal");
    }
    r.ok = true;
    r.http = 202;
    return r;
}

void VotingService::cancelVerify() {
    std::lock_guard<std::mutex> lk(verifyMu_);
    const bool active = vstate_ == VState::Waiting || vstate_ == VState::Sending || vstate_ == VState::Capturing;
    if (!active) return;
    if (send_) send_("CANCEL");
    failVerify("CANCELLED");
}

void VotingService::sendChunk() {   // caller holds verifyMu_
    if (vOffset_ >= vTpl_.size()) {
        char crc[8];
        std::snprintf(crc, sizeof crc, "%X", static_cast<unsigned>(EnrollmentService::crc16(vTpl_)));
        if (!send_ || !send_(std::string("D:END:") + crc)) failVerify("SENSOR");
        vMsg_ = "Place your finger on the sensor.";
        return;
    }
    const size_t n = std::min<size_t>(kChunkBytes, vTpl_.size() - vOffset_);
    if (!send_ || !send_("D:" + std::to_string(vNext_) + ":" + hexEncode(vTpl_.substr(vOffset_, n)))) failVerify("SENSOR");
}

// Called from the serial worker thread with the text after "VF:".
std::string VotingService::onVerifyLine(const std::string& ev) {
    std::lock_guard<std::mutex> lk(verifyMu_);
    const bool active = vstate_ == VState::Waiting || vstate_ == VState::Sending || vstate_ == VState::Capturing;
    if (!active) return "";   // stale chatter (no verification asked for) is ignored
    if (nowMs() > vDeadlineMs_) {
        expireVerifyIfNeeded();
        return "";
    }
    if (ev.rfind("ERR:", 0) == 0) {
        failVerify(ev.substr(4));
        return "";
    }
    if (ev.rfind("READY", 0) == 0 && vstate_ == VState::Waiting) {
        vstate_ = VState::Sending;
        vMsg_ = "Comparing against your enrolled fingerprint...";
        vDeadlineMs_ = nowMs() + kChunkMs;
        sendChunk();
    } else if (ev.rfind("D:", 0) == 0 && vstate_ == VState::Sending) {
        if (std::atoi(ev.c_str() + 2) != vNext_) { failVerify("DOWNLOAD"); return ""; }
        vOffset_ += kChunkBytes;
        ++vNext_;
        vDeadlineMs_ = nowMs() + kChunkMs;
        sendChunk();
    } else if (ev == "PLACE" && vstate_ == VState::Sending) {
        vstate_ = VState::Capturing;
        vMsg_ = "Place your finger on the sensor.";
        vDeadlineMs_ = nowMs() + kCaptureMs;
    } else if (ev.rfind("OK:", 0) == 0 && vstate_ == VState::Capturing) {
        // OK:<score> -- the sensor captured a real finger and matched it against the template we sent from SQLite.
        const std::string t = ev.substr(3);
        bool digits = !t.empty() && t.size() <= 5;
        for (unsigned char c : t) if (!std::isdigit(c)) digits = false;
        const int score = digits ? std::atoi(t.c_str()) : 0;
        if (score < kMinScore) { failVerify(digits ? "LOWSCORE" : "NOMATCH"); return ""; }
        if (onScan(vSlot_) != "ALLOW") {   // re-checks enrolled + not yet voted, then opens the single-use session
            failVerify("INELIGIBLE");
            return "";
        }
        vConfidence_ = score;
        vstate_ = VState::Matched;
        vMsg_ = "Fingerprint matched your enrolled template (score " + std::to_string(score) + ").";
        std::cerr << "[verify] live finger vs database template #" << vSha_.substr(0, 8) << " -> score " << score
                  << ", session opened\n";   // voter identity not logged
        vTpl_.clear();
    }
    return "";
}

std::string VotingService::verifyStatusJson() {
    std::lock_guard<std::mutex> lk(verifyMu_);
    expireVerifyIfNeeded();
    if (vstate_ == VState::Matched && !sessionLive()) vstate_ = VState::Idle;   // ballot window closed or used
    const bool waiting = vstate_ == VState::Waiting || vstate_ == VState::Sending || vstate_ == VState::Capturing;
    const char* name = waiting ? "waiting" : vstate_ == VState::Matched ? "matched"
                       : vstate_ == VState::Failed ? "failed" : "idle";
    const auto s = session();
    return std::string("{\"mode\":\"database-template-match\",\"state\":") + jsonStr(name) +
           ",\"message\":" + jsonStr(vMsg_) +
           ",\"matched\":" + (vstate_ == VState::Matched ? "true" : "false") +
           ",\"failed\":" + (vstate_ == VState::Failed ? "true" : "false") +
           ",\"error_code\":" + jsonStr(vCode_) + ",\"voter_ref\":" + jsonStr(vRef_) +
           ",\"template_sha256\":" + jsonStr(vstate_ == VState::Idle ? "" : vSha_.substr(0, 16)) +
           ",\"confidence\":" + std::to_string(vConfidence_) +
           ",\"session_active\":" + (s.active ? "true" : "false") +
           ",\"remaining_ms\":" + std::to_string(s.remainingMs) + "}";
}

std::string VotingService::castFor(int sensorId, const std::string& candidateName) {
    if (candidateName.empty() || candidateName.size() > 20) return "BAD_CANDIDATE";
    for (unsigned char c : candidateName)
        if (!(std::isalnum(c) || c == '_' || c == ' ')) return "BAD_CANDIDATE";
    auto cand = auth_.candidateByKey(normalizeKey(candidateName));
    if (!cand) return "UNKNOWN_CANDIDATE";   // checked first so a typo does not burn the verified session

    {
        std::lock_guard<std::mutex> lk(sessionMu_);
        const bool live = pendingSensor_ == sensorId && sensorId >= 0 && nowMs() <= deadlineMs_;
        if (!live) return "NOSESSION";       // no session, expired, or it belongs to a different voter
        pendingSensor_ = -1;                 // single-use
        deadlineMs_ = 0;
    }
    return commitVote(sensorId, *cand) ? "ALLOW" : "DENY";
}

bool VotingService::commitVote(int sensorId, const Candidate& cand) {
    std::lock_guard<std::mutex> lk(chainMu_);

    int64_t ts = nowMs();
    if (ts <= lastTs_) ts = lastTs_ + 1;  // strictly increasing timestamps
    const std::string prev = headHash_;
    const std::string hash = chainHash(cand.name, ts, prev);
    const uint64_t seq = ++seq_;

    // 1) in-memory audit entry (lock-free for readers) BEFORE touching the disk
    SkipList::Node* node = audit_.insert(AuditRecord{seq, cand.name, ts, prev, hash});

    // 2) atomic disk commit: voter flag + ledger row
    auto res = db_.commitVote(sensorId, cand.id, cand.name, ts, prev, hash);
    if (!res.ok) {
        if (node) SkipList::setStatus(node, SkipList::Failed);
        std::cerr << "[vote] commit failed: " << res.error << "\n";
        return false;
    }

    // 3) only now advance the chain head and the live structures
    if (node) SkipList::setStatus(node, SkipList::Committed);
    headHash_ = hash;
    lastTs_ = ts;
    auth_.markVoted(sensorId);
    results_.recordVote(cand.id, ts);
    std::cerr << "[vote] ledger #" << res.voteId << " committed\n";  // voter identity never logged
    return true;
}

std::vector<VotingService::AuditView> VotingService::audit(uint64_t from, uint64_t to, size_t limit) const {
    static const char* names[] = {"pending", "committed", "failed"};
    std::vector<AuditView> out;
    for (const auto& v : audit_.range(from, to, limit))
        out.push_back(AuditView{v.rec.seq, v.rec.candidate, v.rec.timestampMs, v.rec.previousHash, v.rec.hash,
                                names[static_cast<int>(v.status)]});
    return out;
}

}  // namespace bvs
