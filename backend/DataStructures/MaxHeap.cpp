#include "MaxHeap.h"

#include <utility>

namespace bvs {

bool MaxHeap::higher(const Entry& a, const Entry& b) {
    if (a.count != b.count) return a.count > b.count;
    return a.id < b.id;
}

void MaxHeap::swapNodes(size_t i, size_t j) {
    std::swap(heap_[i], heap_[j]);
    pos_[heap_[i].id] = i;
    pos_[heap_[j].id] = j;
}

void MaxHeap::siftUp(size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!higher(heap_[i], heap_[parent])) break;
        swapNodes(i, parent);
        i = parent;
    }
}

void MaxHeap::siftDown(size_t i) {
    const size_t n = heap_.size();
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < n && higher(heap_[l], heap_[best])) best = l;
        if (r < n && higher(heap_[r], heap_[best])) best = r;
        if (best == i) break;
        swapNodes(i, best);
        i = best;
    }
}

void MaxHeap::siftDownIn(std::vector<Entry>& h, size_t i) {
    const size_t n = h.size();
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < n && higher(h[l], h[best])) best = l;
        if (r < n && higher(h[r], h[best])) best = r;
        if (best == i) break;
        std::swap(h[i], h[best]);
        i = best;
    }
}

bool MaxHeap::insert(int id, const std::string& name, int64_t count) {
    if (pos_.count(id)) return false;
    heap_.push_back(Entry{id, name, count});
    pos_[id] = heap_.size() - 1;
    total_ += count;
    siftUp(heap_.size() - 1);
    return true;
}

bool MaxHeap::increment(int id, int64_t delta) {
    auto it = pos_.find(id);
    if (it == pos_.end()) return false;
    size_t i = it->second;
    heap_[i].count += delta;
    total_ += delta;
    if (delta >= 0) siftUp(i); else siftDown(i);
    return true;
}

std::vector<MaxHeap::Entry> MaxHeap::topK(size_t k) const {
    std::vector<Entry> work = heap_;  // destructive pops happen on the copy
    std::vector<Entry> out;
    k = std::min(k, work.size());
    out.reserve(k);
    for (size_t n = 0; n < k; ++n) {
        out.push_back(work[0]);
        work[0] = std::move(work.back());
        work.pop_back();
        if (!work.empty()) siftDownIn(work, 0);
    }
    return out;
}

}  // namespace bvs
