// Biometric Voting System - C++ backend entry point.
// REST (cpp-httplib) + SQLite + serial listener for the Arduino/R307S terminal.
#include "httplib.h" // or <httplib.h> if installed system-wide
#include "API/Controllers.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "API/Controllers.h"
#include "Common/Util.h"
#include "Database/DatabaseManager.h"
#include "Hardware/SerialListener.h"
#include "Services/AuthService.h"
#include "Services/EnrollmentService.h"
#include "Services/ResultService.h"
#include "Services/VotingService.h"
#include "Tools/DemoSeeder.h"

#ifndef DEFAULT_DB_PATH
#define DEFAULT_DB_PATH "../database/voting.db"
#endif
#ifndef DEFAULT_SCHEMA_PATH
#define DEFAULT_SCHEMA_PATH "Database/schema.sql"
#endif
#ifndef DEFAULT_FRONTEND_DIR
#define DEFAULT_FRONTEND_DIR "../frontend"
#endif

namespace {
std::atomic<bool> gStop{false};
void onSignal(int) { gStop = true; }

std::string envOr(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : def;
}

std::string randomToken() {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 32; ++i) t.push_back(hex[rd() & 0xf]);
    return t;
}

void usage() {
    std::cout << "Usage: voting_backend [options]\n"
                 "  --port N           HTTP port (default 8080, bound to 127.0.0.1 only)\n"
                 "  --db PATH          SQLite file\n"
                 "  --schema PATH      schema.sql\n"
                 "  --frontend DIR     static files to serve at /\n"
                 "  --serial PORT      e.g. COM3 or /dev/ttyACM0 (env VOTING_SERIAL_PORT)\n"
                 "  --baud N           serial baud (default 9600)\n"
                 "  --no-serial        run API only (development)\n"
                 "  --window-hours N   turnout analytics window (default 24)\n"
                 "  --seed-demo        add candidates ALICE, BOB, CAROL if none exist\n"
                 "  --simulate N       fill an EMPTY db with N demo voters, ~75% turnout and a valid hash chain\n"
                 "  --seed-only        exit after --simulate (use with the sqlite3 CLI)\n"
                 "Env: VOTING_ADMIN_TOKEN, VOTING_SERIAL_PORT, VOTING_PUBLIC_RESULTS=1\n";
}
}  // namespace

int main(int argc, char** argv) {
    int httpPort = 8080;
    std::string dbPath = DEFAULT_DB_PATH;
    std::string schemaPath = DEFAULT_SCHEMA_PATH;
    std::string frontendDir = DEFAULT_FRONTEND_DIR;
#ifdef _WIN32
    std::string serialPort = envOr("VOTING_SERIAL_PORT", "COM3");
#else
    std::string serialPort = envOr("VOTING_SERIAL_PORT", "/dev/ttyACM0");
#endif
    unsigned baud = 9600;
    bool useSerial = true, seedDemo = false, seedOnly = false;
    int simulateN = 0;
    int windowHours = 24;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--port") httpPort = std::atoi(next().c_str());
        else if (a == "--db") dbPath = next();
        else if (a == "--schema") schemaPath = next();
        else if (a == "--frontend") frontendDir = next();
        else if (a == "--serial") serialPort = next();
        else if (a == "--baud") baud = static_cast<unsigned>(std::atoi(next().c_str()));
        else if (a == "--no-serial") useSerial = false;
        else if (a == "--window-hours") windowHours = std::max(1, std::atoi(next().c_str()));
        else if (a == "--seed-demo") seedDemo = true;
        else if (a == "--simulate") simulateN = std::atoi(next().c_str());
        else if (a == "--seed-only") seedOnly = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
    }

    try {
        std::filesystem::path p(dbPath);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());

        // ---- persistence ----
        bvs::DatabaseManager db(dbPath);
        db.applySchema(schemaPath);
        if (simulateN > 0) {
            bvs::seedDemo(db, simulateN);
            std::cout << "[seed] simulated " << simulateN << " voters with a valid hash chain\n";
            if (seedOnly) return 0;
        }

        std::string adminToken = envOr("VOTING_ADMIN_TOKEN", "");
        if (adminToken.empty()) {
            adminToken = randomToken();
            std::cout << "[admin] no VOTING_ADMIN_TOKEN set; generated one for this run:\n        " << adminToken << "\n";
        }

        // ---- services ----
        bvs::AuthService auth(db, adminToken);
        auth.load();
        if (seedDemo && auth.candidates().empty())
            for (const char* n : {"ALICE", "BOB", "CAROL"}) auth.addCandidate(n);

        int64_t electionStart = bvs::nowMs();
        if (auto s = db.getMeta("election_start_ms")) electionStart = std::atoll(s->c_str());
        else db.setMeta("election_start_ms", std::to_string(electionStart));

        bvs::ResultService results(electionStart, 60000, static_cast<size_t>(windowHours) * 60);
        for (const auto& c : auth.candidates()) results.addCandidate(c.id, c.name);

        bvs::VotingService voting(db, auth, results);
        voting.init();
        std::cout << "[boot] voters=" << auth.voterCount() << " candidates=" << auth.candidates().size()
                  << " ledger_entries=" << voting.auditSize() << "\n";

        const auto rep = voting.verifyChain();
        if (!rep.ok) std::cerr << "[SECURITY] hash chain BROKEN at vote id " << rep.badId << "\n";
        else std::cout << "[boot] hash chain verified (" << rep.checked << " votes), head=" << rep.head << "\n";

        // ---- hardware thread (decoupled) ----
        // Lines starting EN:/T: belong to fingerprint enrolment; everything else is the voting protocol.
        std::unique_ptr<bvs::SerialListener> serial;
        bvs::EnrollmentService enroll(
            db, auth, [&serial](const std::string& l) { return serial && serial->sendLine(l); },
            [&serial] { return serial && serial->connected(); }, [&voting] { return voting.session().active; });
        if (useSerial) {
            serial = std::make_unique<bvs::SerialListener>(serialPort, baud, [&voting, &enroll](const std::string& line) {
                return bvs::EnrollmentService::owns(line) ? enroll.handleLine(line) : voting.handleLine(line);
            });
            serial->start();
        }

        // The portal's fingerprint verification talks to the same terminal (VERIFY:<slot> -> VF:... replies).
        voting.setTerminal([&serial](const std::string& l) { return serial && serial->sendLine(l); },
                           [&serial] { return serial && serial->connected(); });

        // ---- HTTP ----
        httplib::Server svr;
        svr.new_task_queue = [] { return new httplib::ThreadPool(8); };
        svr.set_payload_max_length(4096);
        svr.set_read_timeout(5, 0);
        svr.set_write_timeout(5, 0);
        svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            res.status = 500;
            res.set_content("{\"error\":\"internal error\"}", "application/json");
        });
        svr.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
            res.set_header("X-Frame-Options", "DENY");
            res.set_header("Referrer-Policy", "no-referrer");
        });

        const bool publicResults = envOr("VOTING_PUBLIC_RESULTS", "0") == "1";
        bvs::registerAuthRoutes(svr, auth, results, voting, serial.get(), &db);
        bvs::registerVoteRoutes(svr, auth, voting, db);
        bvs::registerResultRoutes(svr, auth, results, publicResults);
        bvs::registerSqlRoutes(svr, auth, db, results);
        bvs::registerEnrollRoutes(svr, auth, db, enroll);

        if (std::filesystem::exists(frontendDir)) svr.set_mount_point("/", frontendDir);

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::thread watcher([&] {
            while (!gStop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            svr.stop();
        });

        std::cout << "[http] listening on http://127.0.0.1:" << httpPort << "  (Ctrl+C to stop)\n";
        if (!svr.listen("127.0.0.1", httpPort)) std::cerr << "[http] failed to bind port " << httpPort << "\n";
        gStop = true;
        watcher.join();
        if (serial) serial->stop();
        std::cout << "[shutdown] ledger head=" << voting.headHash() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
