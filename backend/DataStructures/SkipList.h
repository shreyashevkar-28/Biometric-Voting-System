#pragma once
// Append-only concurrent skip list keyed by a monotonically assigned sequence number.
// Design (why this is thread-safe without a mutex-heavy tree):
//   * Nodes are NEVER removed or relinked after publication -> no ABA / reclamation problem.
//   * Readers (find / range / size) are completely lock-free: they only follow
//     std::atomic<Node*> next-pointers with acquire loads.
//   * Writers are serialized by one tiny mutex held for a single O(log n) splice; a new node is
//     fully built before being published with release stores, bottom level last-visible-safe.
//   insert O(log n) expected   find O(log n) expected   range O(log n + k)
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace bvs {

struct AuditRecord {
    uint64_t seq = 0;
    std::string candidate;
    int64_t timestampMs = 0;
    std::string previousHash;
    std::string hash;
};

class SkipList {
public:
    enum Status : uint8_t { Pending = 0, Committed = 1, Failed = 2 };

    struct Node {
        Node(AuditRecord r, int h) : rec(std::move(r)), height(h), next(new std::atomic<Node*>[h]) {
            for (int i = 0; i < h; ++i) next[i].store(nullptr, std::memory_order_relaxed);
        }
        const AuditRecord rec;                       // immutable after publication
        std::atomic<uint8_t> status{Pending};        // the only mutable field
        const int height;
        std::unique_ptr<std::atomic<Node*>[]> next;
    };

    struct View {
        AuditRecord rec;
        Status status;
    };

    SkipList();
    ~SkipList();
    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // Returns the published node, or nullptr if `seq` already exists.
    Node* insert(AuditRecord rec);
    static void setStatus(Node* n, Status s) { n->status.store(s, std::memory_order_release); }

    bool find(uint64_t seq, View& out) const;
    // Records with from <= seq <= to, ascending, at most `limit`.
    std::vector<View> range(uint64_t from, uint64_t to, size_t limit) const;
    size_t size() const { return size_.load(std::memory_order_acquire); }

private:
    static constexpr int kMaxLevel = 24;

    int randomLevel();
    const Node* lowerBound(uint64_t seq) const;  // first node with rec.seq >= seq

    std::unique_ptr<Node> head_;
    std::atomic<int> level_{1};
    std::atomic<size_t> size_{0};
    std::mutex writeMu_;
    std::mt19937_64 rng_;
};

}  // namespace bvs
