#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "../Database/DatabaseManager.h"
#include "AuthService.h"

namespace bvs {

// Fingerprint enrolment driven by the physical R307S terminal.
//
//   HTTP start -> host sends  ENROLL:<slot>
//   terminal   -> EN:READY:<slot> | EN:PLACE:1 | EN:LIFT | EN:PLACE:2 | EN:PROC | EN:STORED
//                 T:<n>:<hex>  ...  T:END:<bytes>:<crc16>      (template read back from sensor flash)
//                 EN:ERR:<CODE>                               (any failure; terminal cleans up its own slot)
//   host       -> validates length + CRC16, SHA-256, then ONE SQL transaction (voters + biometric_templates).
//                 If that transaction fails the host sends DEL:<slot> so sensor flash and database never disagree.
//
// Matching during the election still runs on the sensor (slot id == voters.sensor_id); the database row is what
// makes that slot an eligible voter, and the stored template is the authoritative backup copy.
class EnrollmentService {
public:
    enum class State { Idle, Starting, Capture1, Lift, Capture2, Processing, Downloading, Saving, Done, Failed };

    using Sender = std::function<bool(const std::string&)>;

    struct StartResult {
        bool ok = false;
        int http = 200;
        std::string error;
        int slot = 0;
    };

    static constexpr int kMinSlot = 1;
    static constexpr int kMaxSlot = 127;   // safe for every R30x variant and Adafruit's own examples

    EnrollmentService(DatabaseManager& db, AuthService& auth, Sender send, std::function<bool()> terminalConnected,
                      std::function<bool()> votingBusy);

    StartResult start(const std::string& voterRef, const std::string& name, int constituencyId,
                      const std::string& finger);
    void cancel();
    std::string statusJson();                     // applies timeouts, then serialises the current state
    std::string handleLine(const std::string& line);   // serial worker thread; always returns ""
    bool removeEnrollment(int sensorId, std::string* error);   // DB + sensor flash

    static bool owns(const std::string& line);    // "EN:..." or "T:..."
    static uint16_t crc16(const std::string& bytes);
    static bool hexDecode(const std::string& hex, std::string& out);
    static bool validVoterRef(const std::string& r);

private:
    void fail(const std::string& code, const std::string& message);   // caller holds mu_
    void setState(State s, const std::string& msg, int64_t timeoutMs);
    void expireIfNeeded();
    static const char* stateName(State s);

    DatabaseManager& db_;
    AuthService& auth_;
    Sender send_;
    std::function<bool()> connected_;
    std::function<bool()> votingBusy_;

    std::mutex mu_;
    State state_ = State::Idle;
    std::string message_;
    std::string errorCode_;
    std::string voterRef_, name_, finger_;
    int constituencyId_ = 0;
    int slot_ = 0;
    int slotRetries_ = 0;
    bool stored_ = false;          // template exists in sensor flash (needs DEL on failure)
    int nextChunk_ = 0;
    std::string tpl_;              // raw bytes accumulated from T: lines
    std::string sha_;
    int64_t startedMs_ = 0;
    int64_t deadlineMs_ = 0;
};

}  // namespace bvs
