#include "DemoSeeder.h"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "../Common/Crypto.h"
#include "../Common/Util.h"

namespace bvs {

void seedDemo(DatabaseManager& db, int voterCount) {
    if (voterCount < 1 || voterCount > 60000) throw std::runtime_error("--simulate N: N must be 1..60000");
    if (db.scalar("SELECT COUNT(*) FROM voters") > 0 || db.scalar("SELECT COUNT(*) FROM votes") > 0)
        throw std::runtime_error("--simulate refuses to run: the database already has voters or votes");

    std::mt19937_64 rng(2026);

    std::vector<int> constituencies;
    for (const char* n : {"North", "South", "East", "West", "Central"}) {
        int id = db.addConstituency(n);
        if (id < 0) throw std::runtime_error("seed: constituency insert failed");
        constituencies.push_back(id);
    }

    auto cands = db.loadCandidates();
    if (cands.empty()) {
        const int p1 = db.addParty("Progress Party");
        const int p2 = db.addParty("Unity Alliance");
        const int p3 = db.addParty("Green Front");
        db.addCandidate("ALICE", p1);
        db.addCandidate("BOB", p2);
        db.addCandidate("CAROL", p3);
        db.addCandidate("DAVE", 0);  // independent
        cands = db.loadCandidates();
    }

    static const char* first[] = {"Aarav", "Diya", "Kabir", "Meera", "Rohan", "Sana", "Vihaan", "Anaya", "Ishaan", "Tara"};
    static const char* last[] = {"Patil", "Kulkarni", "Deshmukh", "Joshi", "Shinde", "Pawar", "Khodwe", "More"};
    for (int i = 1; i <= voterCount; ++i) {
        std::string name = std::string(first[rng() % 10]) + " " + last[rng() % 8] + " #" + std::to_string(i);
        if (!db.addVoter(i, name, constituencies[rng() % constituencies.size()]))
            throw std::runtime_error("seed: voter insert failed");
    }

    // ---- turnout: ~75% of voters, two peaks (morning ~2h, evening ~6h) inside an 8-hour window
    const int64_t end = nowMs();
    const int64_t hour = 3600LL * 1000;
    const int64_t start = end - 8 * hour;
    db.setMeta("election_start_ms", std::to_string(start));

    std::vector<int> ids(voterCount);
    for (int i = 0; i < voterCount; ++i) ids[i] = i + 1;
    std::shuffle(ids.begin(), ids.end(), rng);
    const int voting = voterCount * 3 / 4;
    ids.resize(static_cast<size_t>(voting));

    std::normal_distribution<double> morning(2.0, 0.9), evening(6.0, 1.1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::vector<int64_t> ts(static_cast<size_t>(voting));
    for (auto& t : ts) {
        double h = (coin(rng) < 0.6) ? morning(rng) : evening(rng);
        h = std::min(std::max(h, 0.0), 7.999);
        t = start + static_cast<int64_t>(h * hour);
    }
    std::sort(ts.begin(), ts.end());
    for (size_t i = 1; i < ts.size(); ++i) ts[i] = std::max(ts[i], ts[i - 1] + 1);

    // candidate popularity weights (first four candidates)
    std::vector<double> w;
    const double base[] = {35, 30, 25, 10};
    for (size_t i = 0; i < cands.size(); ++i) w.push_back(i < 4 ? base[i] : 5);
    std::discrete_distribution<size_t> pick(w.begin(), w.end());

    std::string prev = kGenesisHash;
    for (int i = 0; i < voting; ++i) {
        const Candidate& c = cands[pick(rng)];
        const std::string h = chainHash(c.name, ts[static_cast<size_t>(i)], prev);
        auto r = db.commitVote(ids[static_cast<size_t>(i)], c.id, c.name, ts[static_cast<size_t>(i)], prev, h);
        if (!r.ok) throw std::runtime_error("seed: vote commit failed: " + r.error);
        prev = h;
    }
}

}  // namespace bvs
