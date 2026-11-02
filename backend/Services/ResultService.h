#pragma once
#include <cstdint>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "../DataStructures/MaxHeap.h"
#include "../DataStructures/SegmentTree.h"

namespace bvs {

// Live results. Reads never touch SQL.
//   MaxHeap     -> leaderboard   (leader O(1), update O(log n))
//   SegmentTree -> turnout       (range sum O(log n)) over fixed-width time buckets
class ResultService {
public:
    struct Tally {
        int id;
        std::string name;
        int64_t votes;
    };

    // Election window = [startMs, startMs + bucketMs * bucketCount). Votes outside are clamped.
    ResultService(int64_t startMs, int64_t bucketMs, size_t bucketCount);

    void addCandidate(int id, const std::string& name);
    void recordVote(int candidateId, int64_t timestampMs);

    std::vector<Tally> leaderboard(size_t k) const;
    std::optional<Tally> leader() const;
    int64_t totalVotes() const;

    // Votes in [fromMs, toMs) at bucket granularity.
    int64_t turnout(int64_t fromMs, int64_t toMs) const;
    // Consecutive windows of stepMs inside [fromMs, toMs): (windowStartMs, votes).
    std::vector<std::pair<int64_t, int64_t>> timeline(int64_t fromMs, int64_t toMs, int64_t stepMs) const;

    int64_t startMs() const { return startMs_; }
    int64_t bucketMs() const { return bucketMs_; }
    size_t bucketCount() const { return buckets_; }

private:
    size_t lowerBucket(int64_t ms) const;  // first bucket index overlapping ms
    size_t upperBucket(int64_t ms) const;  // exclusive end index covering ms

    const int64_t startMs_;
    const int64_t bucketMs_;
    const size_t buckets_;
    mutable std::shared_mutex mu_;
    MaxHeap heap_;
    SegmentTree tree_;
};

}  // namespace bvs
