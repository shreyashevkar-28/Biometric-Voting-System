// Fingerprint enrolment endpoints. The template bytes never leave the server: responses carry only
// metadata (slot, finger label, SHA-256, timestamps).
#include "ApiCommon.h"
#include "Controllers.h"
#include "../Services/EnrollmentService.h"

namespace bvs {

void registerEnrollRoutes(httplib::Server& svr, AuthService& auth, DatabaseManager& db, EnrollmentService& enroll) {
    // POST /api/enroll/start  voter_ref, name, finger [, constituency_id]   -> 202, then poll /status
    // Public on purpose: the portal has no server-side accounts yet, and the server only listens on 127.0.0.1.
    // Server-side guards: one enrolment at a time, one enrolment per voter_ref, closed once ballots exist.
    svr.Post("/api/enroll/start", [&enroll](const httplib::Request& req, httplib::Response& res) {
        const auto r = enroll.start(paramStr(req, "voter_ref"), paramStr(req, "name"),
                                    static_cast<int>(paramInt(req, "constituency_id", 0)),
                                    paramStr(req, "finger", "Unspecified"));
        if (!r.ok) return sendError(res, r.http, r.error);
        sendJson(res, 202, "{\"started\":true,\"slot\":" + std::to_string(r.slot) + "}");
    });

    // GET /api/enroll/status -> live progress of the current/last enrolment (poll every ~500 ms)
    svr.Get("/api/enroll/status", [&enroll](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, enroll.statusJson());
    });

    // POST /api/enroll/cancel
    svr.Post("/api/enroll/cancel", [&enroll](const httplib::Request&, httplib::Response& res) {
        enroll.cancel();
        sendJson(res, 200, "{\"cancelled\":true}");
    });

    // GET /api/enroll/voter?voter_ref=VTR100001 -> is this portal voter enrolled in the database?
    svr.Get("/api/enroll/voter", [&db](const httplib::Request& req, httplib::Response& res) {
        const std::string ref = paramStr(req, "voter_ref");
        if (!EnrollmentService::validVoterRef(ref)) return sendError(res, 400, "invalid voter_ref");
        const auto b = db.biometricByRef(ref);
        if (!b) return sendJson(res, 200, "{\"enrolled\":false}");
        sendJson(res, 200,
                 "{\"enrolled\":true,\"sensor_id\":" + std::to_string(b->sensorId) +
                     ",\"finger\":" + jsonStr(b->finger) + ",\"template_sha256\":" + jsonStr(b->sha256) +
                     ",\"template_bytes\":" + std::to_string(b->templateBytes) +
                     ",\"enrolled_at_ms\":" + std::to_string(b->enrolledAtMs) +
                     ",\"has_voted\":" + (b->hasVoted ? "true" : "false") + "}");
    });

    // GET /api/enroll/registry (admin) -> all enrolments, metadata only
    svr.Get("/api/enroll/registry", [&auth, &db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        std::string body = "{\"enrolments\":[";
        bool first = true;
        for (const auto& b : db.listBiometrics()) {
            if (!first) body += ",";
            first = false;
            body += "{\"sensor_id\":" + std::to_string(b.sensorId) + ",\"voter_ref\":" + jsonStr(b.voterRef) +
                    ",\"name\":" + jsonStr(b.name) + ",\"finger\":" + jsonStr(b.finger) +
                    ",\"template_sha256\":" + jsonStr(b.sha256) + ",\"enrolled_at_ms\":" + std::to_string(b.enrolledAtMs) +
                    ",\"has_voted\":" + (b.hasVoted ? "true" : "false") + "}";
        }
        sendJson(res, 200, body + "]}");
    });

    // POST /api/enroll/delete (admin) voter_ref=<ref>  -> removes the DB rows and the sensor slot
    svr.Post("/api/enroll/delete", [&auth, &db, &enroll](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const std::string ref = paramStr(req, "voter_ref");
        if (!EnrollmentService::validVoterRef(ref)) return sendError(res, 400, "invalid voter_ref");
        const auto b = db.biometricByRef(ref);
        if (!b) return sendError(res, 404, "no such enrolment");
        std::string err;
        if (!enroll.removeEnrollment(b->sensorId, &err))
            return sendError(res, 409, b->hasVoted ? "voter has already voted; enrolment is locked" : "could not delete: " + err);
        sendJson(res, 200, "{\"deleted\":true}");
    });
}

}  // namespace bvs
