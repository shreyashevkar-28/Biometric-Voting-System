#include "ResultService.h"

#include <algorithm>
#include <mutex>

namespace bvs {

ResultService::ResultService(int64_t startMs, int64_t bucketMs, size_t bucketCount)
    : startMs_(startMs), bucketMs_(bucketMs > 0 ? bucketMs : 60000), buckets_(bucketCount ? bucketCount : 1),
      tree_(bucketCount ? bucketCount : 1) {}

void ResultService::addCandidate(int id, const std::string& name) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    heap_.insert(id, name, 0);
}

void ResultService::recordVote(int candidateId, int64_t timestampMs) {
    size_t idx = 0;
    if (timestampMs > startMs_) idx = std::min<size_t>(static_cast<size_t>((timestampMs - startMs_) / bucketMs_), buckets_ - 1);
    std::unique_lock<std::shared_mutex> lk(mu_);
    heap_.increment(candidateId, 1);   // O(log n)
    tree_.add(idx, 1);                 // O(log n)
}

std::vector<ResultService::Tally> ResultService::leaderboard(size_t k) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    std::vector<Tally> out;
    for (const auto& e : heap_.topK(k)) out.push_back(Tally{e.id, e.name, e.count});
    return out;
}

std::optional<ResultService::Tally> ResultService::leader() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    const MaxHeap::Entry* t = heap_.top();  // O(1)
    if (!t) return std::nullopt;
    return Tally{t->id, t->name, t->count};
}

int64_t ResultService::totalVotes() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return heap_.total();
}

size_t ResultService::lowerBucket(int64_t ms) const {
    if (ms <= startMs_) return 0;
    return std::min<size_t>(static_cast<size_t>((ms - startMs_) / bucketMs_), buckets_);
}

size_t ResultService::upperBucket(int64_t ms) const {
    if (ms <= startMs_) return 0;
    return std::min<size_t>(static_cast<size_t>((ms - startMs_ + bucketMs_ - 1) / bucketMs_), buckets_);
}

int64_t ResultService::turnout(int64_t fromMs, int64_t toMs) const {
    if (toMs <= fromMs) return 0;
    std::shared_lock<std::shared_mutex> lk(mu_);
    return tree_.query(lowerBucket(fromMs), upperBucket(toMs));  // O(log n)
}

std::vector<std::pair<int64_t, int64_t>> ResultService::timeline(int64_t fromMs, int64_t toMs, int64_t stepMs) const {
    std::vector<std::pair<int64_t, int64_t>> out;
    if (stepMs < bucketMs_) stepMs = bucketMs_;
    const int64_t maxPoints = 2000;  // bound the work per request
    for (int64_t t = fromMs; t < toMs && static_cast<int64_t>(out.size()) < maxPoints; t += stepMs)
        out.emplace_back(t, turnout(t, std::min(t + stepMs, toMs)));
    return out;
}

}  // namespace bvs
