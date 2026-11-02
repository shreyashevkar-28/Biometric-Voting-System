#include "ApiCommon.h"
#include "Controllers.h"
#include "../Services/EnrollmentService.h"

namespace bvs {

void registerVoteRoutes(httplib::Server& svr, AuthService& auth, VotingService& voting, DatabaseManager& db) {
    // GET /api/vote/candidates (public)
    svr.Get("/api/vote/candidates", [&db](const httplib::Request&, httplib::Response& res) {
        std::string body = "{\"candidates\":[";
        bool first = true;
        for (const auto& c : db.loadCandidates()) {
            if (!first) body += ",";
            first = false;
            body += "{\"id\":" + std::to_string(c.id) +
                    ",\"name\":" + jsonStr(c.name) +
                    ",\"symbol\":" + jsonStr(c.symbol.empty() ? "🗳️" : c.symbol) +
                    ",\"party\":" + jsonStr(c.partyName.empty() ? "Independent" : c.partyName) +
                    ",\"constituency_id\":" + std::to_string(c.constituencyId) + "}";
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    // POST /api/vote/biometric/start  voter_ref=<ref>
    // Asks the R307S to capture a LIVE finger and search its flash. The session opens only if the sensor reports
    // a match on this voter's own slot (VF:OK:<slot>:<confidence>). Poll /api/vote/biometric/status for progress.
    svr.Post("/api/vote/biometric/start", [&voting, &db](const httplib::Request& req, httplib::Response& res) {
        const std::string ref = paramStr(req, "voter_ref");
        if (!EnrollmentService::validVoterRef(ref)) return sendError(res, 400, "invalid voter_ref");
        const auto b = db.biometricByRef(ref);
        if (!b) return sendError(res, 404, "this voter has no enrolled fingerprint");
        const auto r = voting.startVerify(ref, b->sensorId);
        if (!r.ok) return sendError(res, r.http, r.error);
        sendJson(res, 202, "{\"started\":true}");
    });

    // GET /api/vote/biometric/status -> waiting | matched | failed | idle (+ message, confidence, session timer)
    svr.Get("/api/vote/biometric/status", [&voting](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, voting.verifyStatusJson());
    });

    // POST /api/vote/biometric/cancel
    svr.Post("/api/vote/biometric/cancel", [&voting](const httplib::Request&, httplib::Response& res) {
        voting.cancelVerify();
        sendJson(res, 200, "{\"cancelled\":true}");
    });

    // POST /api/vote/cast  voter_ref=<ref>&candidate=<name>
    // Only valid inside a session opened by a real sensor match for THAT voter. Single use.
    svr.Post("/api/vote/cast", [&voting, &db](const httplib::Request& req, httplib::Response& res) {
        const std::string ref = paramStr(req, "voter_ref");
        if (!EnrollmentService::validVoterRef(ref)) return sendError(res, 400, "invalid voter_ref");
        const auto b = db.biometricByRef(ref);
        if (!b) return sendError(res, 404, "this voter has no enrolled fingerprint");
        const std::string target = paramStr(req, "candidate");
        if (target.empty()) return sendError(res, 400, "candidate name required");

        const std::string reply = voting.castFor(b->sensorId, target);
        if (reply == "NOSESSION")
            return sendError(res, 403, "No verified fingerprint session for this voter. Verify your fingerprint first.");
        if (reply == "UNKNOWN_CANDIDATE") return sendError(res, 404, "That candidate is not on the server ballot.");
        if (reply == "BAD_CANDIDATE") return sendError(res, 400, "invalid candidate name");
        if (reply != "ALLOW") return sendError(res, 409, "Ballot rejected by VotingService");

        sendJson(res, 201, "{\"cast\":true,\"head_hash\":" + jsonStr(voting.headHash()) + "}");
    });

    // GET /api/vote/audit?from=<seq>&to=<seq>&limit=100 (admin; SkipList range scan, O(log n + k))
    svr.Get("/api/vote/audit", [&auth, &voting](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int64_t from = paramInt(req, "from", 0);
        const int64_t to = paramInt(req, "to", INT64_MAX);
        const size_t limit = paramLimit(req, "limit", 100, 1000);
        const auto rows = voting.audit(from < 0 ? 0 : static_cast<uint64_t>(from),
                                       to < 0 ? 0 : static_cast<uint64_t>(to), limit);
        std::string body = "{\"total\":" + std::to_string(voting.auditSize()) + ",\"entries\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            if (i) body += ",";
            body += "{\"seq\":" + std::to_string(r.seq) + ",\"candidate\":" + jsonStr(r.candidate) +
                    ",\"timestamp_ms\":" + std::to_string(r.timestampMs) +
                    ",\"previous_hash\":" + jsonStr(r.previousHash) + ",\"hash\":" + jsonStr(r.hash) +
                    ",\"status\":" + jsonStr(r.status) + "}";
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    // GET /api/vote/verify (admin) -> re-hash the entire on-disk chain
    svr.Get("/api/vote/verify", [&auth, &voting](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const auto rep = voting.verifyChain();
        sendJson(res, rep.ok ? 200 : 409,
                 std::string("{\"chain_valid\":") + (rep.ok ? "true" : "false") +
                     ",\"votes_checked\":" + std::to_string(rep.checked) +
                     ",\"first_bad_id\":" + std::to_string(rep.badId) + ",\"head_hash\":" + jsonStr(rep.head) + "}");
    });
}

}  // namespace bvs