#include "ApiCommon.h"
#include "Controllers.h"

namespace bvs {

void registerResultRoutes(httplib::Server& svr, AuthService& auth, ResultService& results, bool publicLeaderboard) {
    // GET /api/results/leaderboard?k=10 (MaxHeap; no SQL)
    // Admin-only unless VOTING_PUBLIC_RESULTS=1 (live tallies can influence late voters).
    svr.Get("/api/results/leaderboard", [&auth, &results, publicLeaderboard](const httplib::Request& req, httplib::Response& res) {
        if (!publicLeaderboard && !requireAdmin(req, res, auth)) return;
        const size_t k = paramLimit(req, "k", 10, 100);
        const auto rows = results.leaderboard(k);
        std::string body = "{\"total_votes\":" + std::to_string(results.totalVotes()) + ",\"leaderboard\":[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) body += ",";
            body += "{\"rank\":" + std::to_string(i + 1) + ",\"id\":" + std::to_string(rows[i].id) +
                    ",\"name\":" + jsonStr(rows[i].name) + ",\"votes\":" + std::to_string(rows[i].votes) + "}";
        }
        body += "]}";
        sendJson(res, 200, body);
    });

    // GET /api/results/turnout?from=<ms>&to=<ms> (SegmentTree range query, O(log n))
    svr.Get("/api/results/turnout", [&auth, &results](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int64_t from = paramInt(req, "from", results.startMs());
        const int64_t to = paramInt(req, "to", nowMs());
        sendJson(res, 200,
                 "{\"from_ms\":" + std::to_string(from) + ",\"to_ms\":" + std::to_string(to) +
                     ",\"votes\":" + std::to_string(results.turnout(from, to)) +
                     ",\"bucket_ms\":" + std::to_string(results.bucketMs()) + "}");
    });

    // GET /api/results/timeline?from=<ms>&to=<ms>&step_min=<n>
    svr.Get("/api/results/timeline", [&auth, &results](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        const int64_t from = paramInt(req, "from", results.startMs());
        const int64_t to = paramInt(req, "to", nowMs());
        const int64_t stepMin = paramInt(req, "step_min", 5);
        const auto pts = results.timeline(from, to, (stepMin < 1 ? 1 : stepMin) * 60000);
        std::string body = "{\"points\":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) body += ",";
            body += "{\"t_ms\":" + std::to_string(pts[i].first) + ",\"votes\":" + std::to_string(pts[i].second) + "}";
        }
        body += "]}";
        sendJson(res, 200, body);
    });
}

}  // namespace bvs