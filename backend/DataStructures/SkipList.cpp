#include "SkipList.h"

namespace bvs {

SkipList::SkipList() : head_(std::make_unique<Node>(AuditRecord{}, kMaxLevel)), rng_(std::random_device{}()) {}

SkipList::~SkipList() {
    Node* cur = head_->next[0].load(std::memory_order_relaxed);
    while (cur) {
        Node* nxt = cur->next[0].load(std::memory_order_relaxed);
        delete cur;
        cur = nxt;
    }
}

int SkipList::randomLevel() {  // called only under writeMu_
    int lvl = 1;
    uint64_t bits = rng_();
    while ((bits & 1ULL) && lvl < kMaxLevel) {
        ++lvl;
        bits >>= 1;
    }
    return lvl;
}

SkipList::Node* SkipList::insert(AuditRecord rec) {
    std::lock_guard<std::mutex> lk(writeMu_);
    Node* preds[kMaxLevel];
    Node* x = head_.get();
    const int top = level_.load(std::memory_order_relaxed);
    for (int i = top - 1; i >= 0; --i) {
        Node* nx = x->next[i].load(std::memory_order_acquire);
        while (nx && nx->rec.seq < rec.seq) {
            x = nx;
            nx = x->next[i].load(std::memory_order_acquire);
        }
        preds[i] = x;
    }
    Node* nxt0 = preds[0]->next[0].load(std::memory_order_acquire);
    if (nxt0 && nxt0->rec.seq == rec.seq) return nullptr;  // duplicate

    const int h = randomLevel();
    for (int i = top; i < h; ++i) preds[i] = head_.get();

    Node* n = new Node(std::move(rec), h);
    // Fully wire the new node's forward pointers BEFORE any reader can reach it...
    for (int i = 0; i < h; ++i)
        n->next[i].store(preds[i]->next[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    // ...then publish bottom-up with release semantics.
    for (int i = 0; i < h; ++i) preds[i]->next[i].store(n, std::memory_order_release);
    if (h > top) level_.store(h, std::memory_order_release);
    size_.fetch_add(1, std::memory_order_release);
    return n;
}

const SkipList::Node* SkipList::lowerBound(uint64_t seq) const {
    const Node* x = head_.get();
    for (int i = level_.load(std::memory_order_acquire) - 1; i >= 0; --i) {
        const Node* nx = x->next[i].load(std::memory_order_acquire);
        while (nx && nx->rec.seq < seq) {
            x = nx;
            nx = x->next[i].load(std::memory_order_acquire);
        }
    }
    return x->next[0].load(std::memory_order_acquire);
}

bool SkipList::find(uint64_t seq, View& out) const {
    const Node* n = lowerBound(seq);
    if (!n || n->rec.seq != seq) return false;
    out.rec = n->rec;
    out.status = static_cast<Status>(n->status.load(std::memory_order_acquire));
    return true;
}

std::vector<SkipList::View> SkipList::range(uint64_t from, uint64_t to, size_t limit) const {
    std::vector<View> out;
    for (const Node* n = lowerBound(from); n && n->rec.seq <= to && out.size() < limit;
         n = n->next[0].load(std::memory_order_acquire)) {
        out.push_back(View{n->rec, static_cast<Status>(n->status.load(std::memory_order_acquire))});
    }
    return out;
}

}  // namespace bvs
