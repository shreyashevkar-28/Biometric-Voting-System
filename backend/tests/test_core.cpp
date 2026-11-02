// Self-checking tests for the from-scratch data structures and the hash chain.
// Build: see CMake target `core_tests`  (ctest) or compile manually.
#include <atomic>
#include <cassert>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "../Common/Crypto.h"
#include "../DataStructures/MaxHeap.h"
#include "../DataStructures/SegmentTree.h"
#include "../DataStructures/SkipList.h"
#include "../DataStructures/Trie.h"

using namespace bvs;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; return 1; } } while (0)

int main() {
    // --- SHA-256 known vectors ---
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");  // 2-block message

    // --- Trie ---
    Trie t;
    CHECK(t.insert("Alice Johnson", 1) && t.insert("alina", 2) && t.insert("Bob", 3));
    CHECK(!t.insert("ALICE JOHNSON", 9));
    CHECK(t.find("bob").value() == 3 && !t.find("bo").has_value());
    auto ac = t.autocomplete("ali", 10);
    CHECK(ac.size() == 2 && ac[0].first == "Alice Johnson" && ac[1].first == "alina");
    CHECK(t.autocomplete("zzz", 5).empty() && t.autocomplete("", 2).size() == 2);

    // --- MaxHeap vs brute force ---
    MaxHeap h;
    std::mt19937 rng(7);
    std::vector<int64_t> ref(50, 0);
    for (int i = 0; i < 50; ++i) CHECK(h.insert(i, "c" + std::to_string(i)));
    for (int step = 0; step < 5000; ++step) { int id = rng() % 50; h.increment(id); ++ref[id]; }
    auto top = h.topK(50);
    CHECK(top.size() == 50);
    for (size_t i = 0; i + 1 < top.size(); ++i)
        CHECK(top[i].count > top[i + 1].count || (top[i].count == top[i + 1].count && top[i].id < top[i + 1].id));
    for (auto& e : top) CHECK(e.count == ref[e.id]);
    CHECK(h.top()->id == top[0].id && h.total() == 5000);

    // --- SegmentTree vs brute force (non power-of-two size) ---
    SegmentTree st(1000);
    std::vector<int64_t> arr(1000, 0);
    for (int step = 0; step < 20000; ++step) {
        size_t i = rng() % 1000; int64_t d = rng() % 5; st.add(i, d); arr[i] += d;
        if (step % 50 == 0) {
            size_t l = rng() % 1000, r = l + rng() % (1001 - l);
            int64_t want = 0; for (size_t k = l; k < r; ++k) want += arr[k];
            CHECK(st.query(l, r) == want);
        }
    }

    // --- SkipList: 4 writers + 4 lock-free readers concurrently ---
    SkipList sl;
    std::atomic<bool> go{false}, stop{false};
    std::atomic<int> readerErrors{0};
    const int perWriter = 5000;
    std::vector<std::thread> th;
    for (int w = 0; w < 4; ++w)
        th.emplace_back([&, w] {
            while (!go) {}
            for (int i = 0; i < perWriter; ++i) {
                uint64_t seq = static_cast<uint64_t>(i) * 4 + w + 1;
                auto* n = sl.insert(AuditRecord{seq, "X", 0, "p", "h"});
                if (!n) readerErrors++; else SkipList::setStatus(n, SkipList::Committed);
            }
        });
    for (int r = 0; r < 4; ++r)
        th.emplace_back([&] {
            while (!go) {}
            while (!stop) {
                auto v = sl.range(1, 1u << 30, 200);
                for (size_t i = 1; i < v.size(); ++i) if (v[i - 1].rec.seq >= v[i].rec.seq) readerErrors++;
            }
        });
    go = true;
    for (int i = 0; i < 4; ++i) th[i].join();
    stop = true;
    for (size_t i = 4; i < th.size(); ++i) th[i].join();
    CHECK(readerErrors == 0 && sl.size() == 4u * perWriter);
    SkipList::View v;
    CHECK(sl.find(1, v) && v.status == SkipList::Committed && !sl.find(999999, v));
    CHECK(sl.insert(AuditRecord{1, "dup", 0, "", ""}) == nullptr);
    auto rg = sl.range(10, 20, 100);
    CHECK(rg.size() == 11 && rg.front().rec.seq == 10 && rg.back().rec.seq == 20);

    std::cout << "ALL CORE TESTS PASSED\n";
    return 0;
}
