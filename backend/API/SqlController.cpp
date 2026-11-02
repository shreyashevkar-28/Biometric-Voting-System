// SQL reporting endpoints. Every statement is a compile-time constant (whitelist); nothing the
// client sends is ever concatenated into SQL. This is the RELATIONAL twin of the in-memory
// data structures, and /api/sql/consistency proves the two agree and measures both.
#include <chrono>
#include <map>

#include "ApiCommon.h"
#include "Controllers.h"

namespace bvs {
namespace {

const std::map<std::string, std::string>& reports() {
    static const std::map<std::string, std::string> r = {
        {"leaderboard", "SELECT * FROM v_leaderboard"},
        {"party-tally", "SELECT * FROM v_party_tally"},
        {"constituency-turnout", "SELECT * FROM v_constituency_turnout"},
        {"hourly-turnout", "SELECT * FROM v_hourly_turnout"},
        {"broken-chain-links", "SELECT * FROM v_chain_links WHERE link_ok = 0"},
        {"voter-auth-status", "SELECT * FROM v_voter_auth_status ORDER BY sensor_id ASC"},
        {"biometric-scans", "SELECT * FROM biometric_scans ORDER BY scan_id DESC LIMIT 200"},
        {"remaining-voters",
         "SELECT k.name AS constituency, COUNT(*) AS not_yet_voted FROM voters v "
         "JOIN constituencies k ON k.constituency_id = v.constituency_id "
         "WHERE v.has_voted = 0 GROUP BY k.constituency_id ORDER BY not_yet_voted DESC"},
        {"audit-log", "SELECT event_id, ts_ms, event_type, detail FROM audit_log ORDER BY event_id DESC LIMIT 200"},
    };
    return r;
}

template <typename F>
double avgMicros(int iterations, F&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) fn();
    const auto dt = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return dt / iterations;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

}  // namespace

void registerSqlRoutes(httplib::Server& svr, AuthService& auth, DatabaseManager& db, ResultService& results) {
    // GET /api/sql/reports -> available report names
    svr.Get("/api/sql/reports", [&auth](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        std::string body = "{\"reports\":[";
        bool first = true;
        for (const auto& kv : reports()) {
            if (!first) body += ",";
            first = false;
            body += jsonStr(kv.first);
        }
        sendJson(res, 200, body + "]}");
    });

    // GET /api/sql/report/<name> -> rows from a whitelisted SQL view/query
    svr.Get(R"(/api/sql/report/([a-z\-]+))", [&auth, &db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        auto it = reports().find(req.matches[1]);
        if (it == reports().end()) return sendError(res, 404, "unknown report");
        sendJson(res, 200, "{\"report\":" + jsonStr(it->first) + ",\"sql\":" + jsonStr(it->second) +
                               ",\"rows\":" + db.queryJson(it->second) + "}");
    });

    // GET /api/sql/explain/<name> -> EXPLAIN QUERY PLAN (shows which B-Tree / index SQLite uses)
    svr.Get(R"(/api/sql/explain/([a-z\-]+))", [&auth, &db](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;
        auto it = reports().find(req.matches[1]);
        if (it == reports().end()) return sendError(res, 404, "unknown report");
        sendJson(res, 200, "{\"report\":" + jsonStr(it->first) + ",\"plan\":" +
                               db.queryJson("EXPLAIN QUERY PLAN " + it->second) + "}");
    });

    // GET /api/sql/consistency -> do heap/segment-tree answers equal SQL answers? how fast is each?
    svr.Get("/api/sql/consistency", [&auth, &db, &results](const httplib::Request& req, httplib::Response& res) {
        if (!requireAdmin(req, res, auth)) return;

        // --- leaderboard: MaxHeap vs SQL GROUP BY / ORDER BY
        const auto sqlRows = db.sqlTallies();
        const auto heapRows = results.leaderboard(sqlRows.size() + 1);
        bool boardMatch = heapRows.size() == sqlRows.size();
        for (size_t i = 0; boardMatch && i < sqlRows.size(); ++i)
            boardMatch = heapRows[i].id == sqlRows[i].first && heapRows[i].votes == sqlRows[i].second;

        // --- turnout: SegmentTree range query vs SQL COUNT(*) over the same bucket-aligned range
        const int64_t start = results.startMs(), bucket = results.bucketMs();
        const int64_t span = results.bucketCount() * bucket;
        int64_t elapsed = nowMs() - start;
        if (elapsed < 0) elapsed = 0;
        int64_t alignedTo = start + ((elapsed + bucket - 1) / bucket) * bucket;
        if (alignedTo > start + span) alignedTo = start + span;
        const int64_t treeVotes = results.turnout(start, alignedTo);
        const int64_t sqlVotes = db.sqlVotesBetween(start, alignedTo);

        // --- timing (average microseconds per operation)
        volatile int64_t sink = 0;
        const double heapUs = avgMicros(2000, [&] { sink = sink + static_cast<int64_t>(results.leaderboard(sqlRows.size()).size()); });
        const double sqlBoardUs = avgMicros(200, [&] { sink = sink + static_cast<int64_t>(db.sqlTallies().size()); });
        const double treeUs = avgMicros(5000, [&] { sink = sink + results.turnout(start, alignedTo); });
        const double sqlRangeUs = avgMicros(200, [&] { sink = sink + db.sqlVotesBetween(start, alignedTo); });
        (void)sink;

        sendJson(res, 200,
                 std::string("{\"leaderboard_match\":") + (boardMatch ? "true" : "false") +
                     ",\"turnout_match\":" + ((treeVotes == sqlVotes) ? "true" : "false") +
                     ",\"segment_tree_votes\":" + std::to_string(treeVotes) +
                     ",\"sql_votes\":" + std::to_string(sqlVotes) +
                     ",\"heap_total\":" + std::to_string(results.totalVotes()) +
                     ",\"sql_total\":" + std::to_string(db.scalar("SELECT COUNT(*) FROM votes")) +
                     ",\"avg_microseconds\":{\"maxheap_leaderboard\":" + fmt(heapUs) +
                     ",\"sql_group_by_leaderboard\":" + fmt(sqlBoardUs) +
                     ",\"segment_tree_range\":" + fmt(treeUs) +
                     ",\"sql_count_range\":" + fmt(sqlRangeUs) + "}}");
    });
}

}  // namespace bvs