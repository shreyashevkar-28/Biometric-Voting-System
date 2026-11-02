#pragma once
// Indexed binary max-heap of candidate tallies (higher votes first, ties -> lower id).
//   top()        O(1)           current leader
//   increment()  O(log n)       position map gives O(1) lookup, then sift-up
//   topK(k)      O(n + k log n) copies the array once, pops k times (n = #candidates, tiny)
// NOT internally synchronized: ResultService guards it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bvs {

class MaxHeap {
public:
    struct Entry {
        int id;
        std::string name;
        int64_t count;
    };

    bool insert(int id, const std::string& name, int64_t count = 0);  // false if id exists
    bool increment(int id, int64_t delta = 1);                        // false if id unknown
    const Entry* top() const { return heap_.empty() ? nullptr : &heap_[0]; }
    std::vector<Entry> topK(size_t k) const;
    size_t size() const { return heap_.size(); }
    int64_t total() const { return total_; }

private:
    std::vector<Entry> heap_;
    std::unordered_map<int, size_t> pos_;
    int64_t total_ = 0;

    static bool higher(const Entry& a, const Entry& b);
    void swapNodes(size_t i, size_t j);
    void siftUp(size_t i);
    void siftDown(size_t i);
    static void siftDownIn(std::vector<Entry>& h, size_t i);
};

}  // namespace bvs
