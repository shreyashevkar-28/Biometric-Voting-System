#include "Trie.h"

#include <map>

#include "../Common/Util.h"

namespace bvs {

struct Trie::Node {
    std::map<unsigned char, std::unique_ptr<Node>> next;
    bool terminal = false;
    int id = 0;
    std::string display;  // original casing, returned to callers
};

Trie::Trie() : root_(std::make_unique<Node>()) {}
Trie::~Trie() = default;

bool Trie::insert(const std::string& word, int id) {
    if (word.empty()) return false;
    Node* cur = root_.get();
    for (unsigned char c : toLowerAscii(word)) {
        auto& slot = cur->next[c];
        if (!slot) slot = std::make_unique<Node>();
        cur = slot.get();
    }
    if (cur->terminal) return false;
    cur->terminal = true;
    cur->id = id;
    cur->display = word;
    ++size_;
    return true;
}

const Trie::Node* Trie::locate(const std::string& key) const {
    const Node* cur = root_.get();
    for (unsigned char c : toLowerAscii(key)) {
        auto it = cur->next.find(c);
        if (it == cur->next.end()) return nullptr;
        cur = it->second.get();
    }
    return cur;
}

std::optional<int> Trie::find(const std::string& word) const {
    const Node* n = locate(word);
    if (n && n->terminal) return n->id;
    return std::nullopt;
}

void Trie::collect(const Node* n, std::vector<std::pair<std::string, int>>& out, size_t limit) {
    if (out.size() >= limit) return;
    if (n->terminal) out.emplace_back(n->display, n->id);
    for (const auto& kv : n->next) {
        if (out.size() >= limit) return;
        collect(kv.second.get(), out, limit);
    }
}

std::vector<std::pair<std::string, int>> Trie::autocomplete(const std::string& prefix, size_t limit) const {
    std::vector<std::pair<std::string, int>> out;
    if (limit == 0) return out;
    const Node* start = locate(prefix);
    if (start) collect(start, out, limit);
    return out;
}

}  // namespace bvs
