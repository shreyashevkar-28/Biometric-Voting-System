#include "EnrollmentService.h"

#include <cctype>
#include <cstdlib>
#include <iostream>

#include "../Common/Crypto.h"
#include "../Common/Util.h"

namespace bvs {
namespace {
constexpr int64_t kStartAckMs = 8000;       // terminal must answer ENROLL quickly
constexpr int64_t kStepMs = 45000;          // per terminal event (the terminal has its own 30 s finger timeout)
constexpr int64_t kDownloadMs = 25000;
constexpr size_t kMinTemplate = 64, kMaxTemplate = 1024;
constexpr int kMaxSlotRetries = 8;

const char* messageFor(const std::string& code) {
    if (code == "TIMEOUT") return "No finger detected in time. Please try again.";
    if (code == "IMAGE") return "Could not read the fingerprint image. Clean the sensor and try again.";
    if (code == "MISMATCH") return "The two scans did not match. Use the same finger both times.";
    if (code == "DUPLICATE") return "This fingerprint is already enrolled.";
    if (code == "STORE") return "The sensor could not save the template.";
    if (code == "DOWNLOAD") return "Could not read the template back from the sensor.";
    if (code == "SENSOR") return "Fingerprint sensor not responding. Check the wiring.";
    if (code == "BUSY") return "The terminal is busy. Try again in a moment.";
    if (code == "FULL") return "The sensor's template memory is full.";
    return "Enrolment failed.";
}
}  // namespace

EnrollmentService::EnrollmentService(DatabaseManager& db, AuthService& auth, Sender send,
                                     std::function<bool()> terminalConnected, std::function<bool()> votingBusy)
    : db_(db), auth_(auth), send_(std::move(send)), connected_(std::move(terminalConnected)),
      votingBusy_(std::move(votingBusy)) {}

bool EnrollmentService::owns(const std::string& line) {
    return line.rfind("EN:", 0) == 0 || line.rfind("T:", 0) == 0;
}

uint16_t EnrollmentService::crc16(const std::string& bytes) {   // CRC-16/CCITT-FALSE, same routine as the firmware
    uint16_t crc = 0xFFFF;
    for (unsigned char b : bytes) {
        crc ^= static_cast<uint16_t>(b) << 8;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

bool EnrollmentService::hexDecode(const std::string& hex, std::string& out) {
    if (hex.empty() || hex.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool EnrollmentService::validVoterRef(const std::string& r) {
    if (r.empty() || r.size() > 32) return false;
    for (unsigned char c : r)
        if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
}

const char* EnrollmentService::stateName(State s) {
    switch (s) {
        case State::Idle: return "idle";
        case State::Starting: return "starting";
        case State::Capture1: return "capture1";
        case State::Lift: return "lift";
        case State::Capture2: return "capture2";
        case State::Processing: return "processing";
        case State::Downloading: return "downloading";
        case State::Saving: return "saving";
        case State::Done: return "done";
        case State::Failed: return "failed";
    }
    return "idle";
}

void EnrollmentService::setState(State s, const std::string& msg, int64_t timeoutMs) {
    state_ = s;
    message_ = msg;
    deadlineMs_ = nowMs() + timeoutMs;
}

void EnrollmentService::fail(const std::string& code, const std::string& message) {
    if (stored_) send_("DEL:" + std::to_string(slot_));   // only a template WE caused to be stored; best effort
    stored_ = false;
    errorCode_ = code;
    state_ = State::Failed;
    message_ = message;
    tpl_.clear();
    std::cerr << "[enroll] failed: " << code << "\n";
}

void EnrollmentService::expireIfNeeded() {   // caller holds mu_
    const bool active = state_ != State::Idle && state_ != State::Done && state_ != State::Failed;
    if (active && nowMs() > deadlineMs_) {
        send_("CANCEL");
        fail("TIMEOUT", state_ == State::Starting ? "The fingerprint terminal did not respond."
                                                   : "Enrolment timed out. Please try again.");
    }
}

EnrollmentService::StartResult EnrollmentService::start(const std::string& voterRef, const std::string& name,
                                                        int constituencyId, const std::string& finger) {
    StartResult r;
    auto bad = [&](int http, const char* msg) { r.http = http; r.error = msg; return r; };

    if (!validVoterRef(voterRef)) return bad(400, "voter_ref must be 1-32 characters of A-Z a-z 0-9 _ -");
    if (!AuthService::validVoterName(name)) return bad(400, "invalid voter name");
    if (finger.empty() || finger.size() > 32) return bad(400, "finger label required (max 32 characters)");
    for (unsigned char c : finger)
        if (!(std::isalpha(c) || c == ' ')) return bad(400, "finger label may only contain letters and spaces");

    std::lock_guard<std::mutex> lk(mu_);
    expireIfNeeded();
    if (state_ != State::Idle && state_ != State::Done && state_ != State::Failed)
        return bad(409, "another enrolment is in progress");
    if (!connected_()) return bad(503, "fingerprint terminal is not connected");
    if (votingBusy_()) return bad(409, "a voting session is open on the terminal");
    if (db_.scalar("SELECT COUNT(*) FROM votes") > 0) return bad(409, "voting has started; enrolment is closed");
    if (db_.biometricByRef(voterRef)) return bad(409, "this voter is already enrolled");
    const int slot = db_.lowestFreeSlot(kMinSlot, kMaxSlot);
    if (slot < 0) return bad(507, "no free fingerprint slots");

    voterRef_ = voterRef;
    name_ = name;
    finger_ = finger;
    constituencyId_ = constituencyId;
    slot_ = slot;
    slotRetries_ = 0;
    stored_ = false;
    nextChunk_ = 0;
    tpl_.clear();
    sha_.clear();
    errorCode_.clear();
    startedMs_ = nowMs();
    setState(State::Starting, "Starting the fingerprint sensor...", kStartAckMs);

    if (!send_("ENROLL:" + std::to_string(slot))) {
        fail("SENSOR", "Could not talk to the fingerprint terminal.");
        return bad(503, "could not write to the terminal");
    }
    r.ok = true;
    r.http = 202;
    r.slot = slot;
    return r;
}

void EnrollmentService::cancel() {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == State::Idle || state_ == State::Done || state_ == State::Failed) return;
    send_("CANCEL");
    fail("CANCELLED", "Enrolment cancelled.");
}

bool EnrollmentService::removeEnrollment(int sensorId, std::string* error) {
    if (!auth_.removeEnrollment(sensorId, error)) return false;
    send_("DEL:" + std::to_string(sensorId));   // best effort: the DB is the source of truth for eligibility
    return true;
}

std::string EnrollmentService::handleLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(mu_);
    const bool active = state_ != State::Idle && state_ != State::Done && state_ != State::Failed;
    if (!active) return "";   // stale chatter after a timeout / cancel is ignored

    if (line.rfind("EN:", 0) == 0) {
        const std::string ev = line.substr(3);
        if (ev.rfind("READY:", 0) == 0) {
            setState(State::Capture1, "Place your finger on the sensor.", kStepMs);
        } else if (ev == "PLACE:1") {
            setState(State::Capture1, "Place your finger on the sensor.", kStepMs);
        } else if (ev == "LIFT") {
            setState(State::Lift, "Good. Now lift your finger.", kStepMs);
        } else if (ev == "PLACE:2") {
            setState(State::Capture2, "Place the same finger again.", kStepMs);
        } else if (ev == "PROC") {
            setState(State::Processing, "Creating the fingerprint template...", kStepMs);
        } else if (ev == "STORED") {
            stored_ = true;
            nextChunk_ = 0;
            tpl_.clear();
            setState(State::Downloading, "Reading the template from the sensor...", kDownloadMs);
        } else if (ev.rfind("ERR:", 0) == 0) {
            const std::string code = ev.substr(4);
            if (code == "SLOT_USED" && slotRetries_ < kMaxSlotRetries) {   // orphan template on the sensor: skip it
                ++slotRetries_;
                const int next = db_.lowestFreeSlot(slot_ + 1, kMaxSlot);
                if (next > 0) {
                    slot_ = next;
                    setState(State::Starting, "Starting the fingerprint sensor...", kStartAckMs);
                    send_("ENROLL:" + std::to_string(slot_));
                    return "";
                }
                return fail("FULL", messageFor("FULL")), "";
            }
            stored_ = false;   // the terminal removes its own slot before reporting an error
            slot_ = 0;
            fail(code, messageFor(code));
        }
        return "";
    }

    if (line.rfind("T:", 0) == 0 && state_ == State::Downloading) {
        deadlineMs_ = nowMs() + kDownloadMs;
        if (line.rfind("T:END:", 0) == 0) {
            const std::string rest = line.substr(6);
            const auto colon = rest.find(':');
            if (colon == std::string::npos) return fail("DOWNLOAD", messageFor("DOWNLOAD")), "";
            const long declared = std::strtol(rest.substr(0, colon).c_str(), nullptr, 10);
            const unsigned long crc = std::strtoul(rest.substr(colon + 1).c_str(), nullptr, 16);
            if (declared != static_cast<long>(tpl_.size()) || tpl_.size() < kMinTemplate ||
                tpl_.size() > kMaxTemplate || crc != crc16(tpl_)) {
                std::cerr << "[enroll] template integrity check failed (got " << tpl_.size() << " bytes)\n";
                return fail("DOWNLOAD", messageFor("DOWNLOAD")), "";
            }
            setState(State::Saving, "Saving to the election database...", kStepMs);
            sha_ = sha256Hex(tpl_);
            std::string err;
            if (!auth_.enrollWithTemplate(slot_, name_, constituencyId_, voterRef_, finger_, tpl_, sha_, &err)) {
                std::cerr << "[enroll] database refused: " << err << "\n";
                return fail("DB", "The database rejected this enrolment: " + err), "";
            }
            stored_ = false;   // now owned by the database row
            tpl_.clear();
            state_ = State::Done;
            message_ = "Fingerprint enrolled.";
            std::cerr << "[enroll] slot " << slot_ << " stored (" << declared << " bytes)\n";   // no name in logs
            return "";
        }
        const auto c2 = line.find(':', 2);
        if (c2 == std::string::npos) return fail("DOWNLOAD", messageFor("DOWNLOAD")), "";
        const int idx = std::atoi(line.substr(2, c2 - 2).c_str());
        if (idx != nextChunk_ || !hexDecode(line.substr(c2 + 1), tpl_) || tpl_.size() > kMaxTemplate)
            return fail("DOWNLOAD", messageFor("DOWNLOAD")), "";
        ++nextChunk_;
    }
    return "";
}

std::string EnrollmentService::statusJson() {
    std::lock_guard<std::mutex> lk(mu_);
    expireIfNeeded();
    std::string o = "{\"state\":" + jsonStr(stateName(state_)) + ",\"message\":" + jsonStr(message_) +
                    ",\"active\":" + ((state_ != State::Idle && state_ != State::Done && state_ != State::Failed) ? "true" : "false") +
                    ",\"done\":" + (state_ == State::Done ? "true" : "false") +
                    ",\"failed\":" + (state_ == State::Failed ? "true" : "false") +
                    ",\"error_code\":" + jsonStr(errorCode_) + ",\"voter_ref\":" + jsonStr(voterRef_);
    if (state_ == State::Done) {
        o += ",\"sensor_id\":" + std::to_string(slot_) + ",\"finger\":" + jsonStr(finger_) +
             ",\"template_sha256\":" + jsonStr(sha_);
    }
    o += ",\"elapsed_ms\":" + std::to_string(startedMs_ ? nowMs() - startedMs_ : 0) + "}";
    return o;
}

}  // namespace bvs
