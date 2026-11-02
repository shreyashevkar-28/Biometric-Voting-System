#include <cstdio>
#include "ApiCommon.h"
#include "Controllers.h"

namespace bvs {

void registerAuthRoutes(httplib::Server& svr, AuthService& auth, ResultService& results, VotingService& voting,
                        const SerialListener* serial, DatabaseManager* db) {
    // GET /api/auth/session -> state of the voting booth (public, no identities)
    svr.Get("/api/auth/session", [&voting, serial](const httplib::Request&, httplib::Response& res) {
        const auto s = voting.session();
        sendJson(res, 200,
                 std::string("{\"session_active\":") + (s.active ? "true" : "false") +
                     ",\"remaining_ms\":" + std::to_string(s.remainingMs) +
                     ",\"hardware_connected\":" + ((serial && serial->connected()) ? "true" : "false") + "}");
    });

    // GET /api/auth/search?type=voter|candidate&q=<prefix>&limit=10 (Trie, O(L))
    svr.Get("/api/auth/search", [&auth](const httplib::Request& req, httplib::Response& res) {
        const std::string type = req.has_param("type") ? req.get_param_value("type") : "candidate";
        const std::string q = req.get_param_value("q");
        const size_t limit = paramLimit(req, "limit", 10, 50);
        if (q.size() > 64) return sendError(res, 400, "query too long");

        std::vector<std::pair<std::string, int>> hits;
        if (type == "voter") {
            if (!requireAdmin(req, res, auth)) return;  // voter names are personal data
            hits = auth.searchVoters(q, limit);
        } else if (type == "candidate") {
            hits = auth.searchCandidates(q, limit);
        } else {
            return sendError(res, 400, "type must be voter or candidate");
        }
        std::string body = "{\"type\":" + jsonStr(type) + ",\"results\":[";
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i) body += ",";
            body += "{\"name\":" + jsonStr(hits[i].first) + ",\"id\":" + std::to_string(hits[i].second) + "}";
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    // POST /api/auth/enroll (admin)
    // Accepts sensor_id, name, constituency_id, plus optional epic_no and template_sig from R307S
    svr.Post("/api/auth/enroll", [&auth, db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int64_t sid = paramInt(req, "sensor_id", -1);
        const std::string name = paramStr(req, "name");
        if (sid < 0 || sid > 65535) return sendError(res, 400, "sensor_id must be 0..65535");
        const int64_t kid = paramInt(req, "constituency_id", 0);

        if (!auth.enrollVoter(static_cast<int>(sid), name, static_cast<int>(kid)))
            return sendError(res, 409, "invalid name/constituency, or sensor_id already enrolled");

        // Generate or store R307S optical template signature & EPIC number in SQLite v2.1 columns
        char defaultEpic[32], defaultSig[64];
        std::snprintf(defaultEpic, sizeof(defaultEpic), "EPIC-%05lld", static_cast<long long>(sid));
        std::snprintf(defaultSig, sizeof(defaultSig), "R307S-SLOT-%04llX-%08llX",
                      static_cast<unsigned long long>(sid),
                      static_cast<unsigned long long>((sid * 2654435761ULL) & 0xFFFFFFFFULL));

        const std::string epicNo = paramStr(req, "epic_no", defaultEpic);
        const std::string templateSig = paramStr(req, "template_sig", defaultSig);

        if (db) {
            db->queryJson("UPDATE voters SET epic_no = " + sqlQuote(epicNo) +
                          ", template_sig = " + sqlQuote(templateSig) +
                          " WHERE sensor_id = " + std::to_string(sid));
            db->queryJson("INSERT INTO biometric_scans(sensor_id, confidence, scan_status) VALUES (" +
                          std::to_string(sid) + ", 100, 'ENROLLED')");
        }

        sendJson(res, 201,
                 "{\"enrolled\":true,\"sensor_id\":" + std::to_string(sid) +
                     ",\"name\":" + jsonStr(name) +
                     ",\"epic_no\":" + jsonStr(epicNo) +
                     ",\"template_sig\":" + jsonStr(templateSig) + "}");
    });

    // POST /api/auth/biometric/scan
    // Cross-checks R307S sensor_id against SQLite `v_voter_auth_status` and opens the hardware voting session
    // ADMIN ONLY: the sensor id comes from the HTTP client, not from the sensor, so it must never be reachable by a
    // voter's browser (it would open a voting session without any fingerprint). Voters use /api/vote/biometric/*.
    svr.Post("/api/auth/biometric/scan", [&auth, &voting, db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int64_t sid = paramInt(req, "sensor_id", -1);
        const int64_t confidence = paramInt(req, "confidence", 145);
        if (sid < 0 || sid > 65535) return sendError(res, 400, "sensor_id must be 0..65535");

        // Trigger the exact serial protocol command expected by VotingService
        const std::string reply = voting.handleLine("SCAN:" + std::to_string(sid));
        const auto s = voting.session();

        // Cross-check SQLite voter status if DatabaseManager pointer is wired
        std::string voterRows = "[]";
        if (db) {
            voterRows = db->queryJson(
                "SELECT sensor_id, epic_no, voter_name, constituency, template_sig, has_voted, auth_status "
                "FROM v_voter_auth_status WHERE sensor_id = " + std::to_string(sid));

            std::string scanStatus = s.active ? "MATCH_OK" : "DUPLICATE_BLOCKED";
            if (voterRows == "[]") scanStatus = "NO_MATCH";
            db->queryJson("INSERT INTO biometric_scans(sensor_id, confidence, scan_status) VALUES (" +
                          std::to_string(sid) + ", " + std::to_string(confidence) + ", " + sqlQuote(scanStatus) + ")");
            if (s.active) {
                db->queryJson("UPDATE voters SET confidence_last = " + std::to_string(confidence) +
                              " WHERE sensor_id = " + std::to_string(sid));
            }
        }

        if (!s.active) {
            sendJson(res, 403,
                     "{\"matched\":false,\"session_active\":false,\"serial_reply\":" + jsonStr(reply) +
                         ",\"voter_sql\":" + voterRows + "}");
            return;
        }

        sendJson(res, 200,
                 "{\"matched\":true,\"session_active\":true,\"sensor_id\":" + std::to_string(sid) +
                     ",\"confidence\":" + std::to_string(confidence) +
                     ",\"remaining_ms\":" + std::to_string(s.remainingMs) +
                     ",\"serial_reply\":" + jsonStr(reply) +
                     ",\"voter_sql\":" + voterRows + "}");
    });

    // POST /api/auth/candidates (admin) name=<text>[&party=<text>&symbol=<emoji>]
    svr.Post("/api/auth/candidates", [&auth, &results, db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const std::string name = paramStr(req, "name");
        int partyId = static_cast<int>(paramInt(req, "party_id", 0));
        const std::string party = paramStr(req, "party");
        const std::string symbol = paramStr(req, "symbol", "🗳️");

        if (name.empty()) return sendError(res, 400, "candidate name required");
        if (partyId == 0 && !party.empty() && db) {
            partyId = db->addParty(party);
        }

        const int id = auth.addCandidate(name, partyId, symbol);
        if (id < 0) return sendError(res, 409, "invalid or duplicate candidate name");
        results.addCandidate(id, name);

        sendJson(res, 201,
                 "{\"id\":" + std::to_string(id) + ",\"name\":" + jsonStr(name) +
                     ",\"party\":" + jsonStr(party.empty() ? "Independent" : party) +
                     ",\"symbol\":" + jsonStr(symbol) + "}");
    });

    // POST /api/election/reset & /api/vote/reset
    // Clears ballots and resets voter has_voted status; strictly PRESERVES all biometric templates and voter accounts!
    auto handleReset = [&auth, &voting, db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        if (!db) return sendError(res, 501, "database reference not wired in main.cpp");
        std::string err;
        if (!db->resetElection(&err)) return sendError(res, 500, "Reset failed: " + err);
        voting.resetLedger();
        sendJson(res, 200, "{\"reset\":true,\"message\":\"Polls reset successfully. All voter biometric templates are preserved.\"}");
    };
    svr.Post("/api/election/reset", handleReset);
    svr.Post("/api/vote/reset", handleReset);

    // POST /api/auth/candidates/delete (admin) id=<n>
    // Safely removes a 0-vote candidate from SQLite (protected by trigger candidates_no_delete_with_votes)
    svr.Post("/api/auth/candidates/delete", [&auth, db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        if (!db) return sendError(res, 501, "Database reference not wired in main.cpp");
        const int64_t cid = paramInt(req, "id", -1);
        if (cid <= 0) return sendError(res, 400, "valid candidate id required");

        const int64_t voteCount = db->scalar("SELECT COUNT(*) FROM votes WHERE candidate_id = " + std::to_string(cid));
        if (voteCount > 0) {
            return sendError(res, 409, "Cannot delete candidate after ballots have been cast for them");
        }

        db->queryJson("DELETE FROM candidates WHERE id = " + std::to_string(cid));
        auth.load(); // Reload in-memory candidate cache from SQLite
        sendJson(res, 200, "{\"deleted\":true,\"id\":" + std::to_string(cid) + "}");
    });

    // POST /api/auth/parties (admin) name=<text>
    svr.Post("/api/auth/parties", [&auth](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int id = auth.addParty(paramStr(req, "name"));
        if (id < 0) return sendError(res, 409, "invalid or duplicate party");
        sendJson(res, 201, "{\"party_id\":" + std::to_string(id) + "}");
    });

    // POST /api/auth/constituencies (admin) name=<text>
    svr.Post("/api/auth/constituencies", [&auth](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int id = auth.addConstituency(paramStr(req, "name"));
        if (id < 0) return sendError(res, 409, "invalid or duplicate constituency");
        sendJson(res, 201, "{\"constituency_id\":" + std::to_string(id) + "}");
    });
}

}  // namespace bvs