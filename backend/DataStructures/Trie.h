#pragma once
// Prefix tree: insert / exact find in O(L), autocomplete in O(L + k) for k results returned.
// Case-insensitive (ASCII). Children are ordered so autocomplete is alphabetical.
// NOT internally synchronized: the owning service guards it with a shared_mutex.
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bvs {

class Trie {
public:
    Trie();
    ~Trie();
    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;

    // Returns false if the (case-insensitive) word already exists.
    bool insert(const std::string& word, int id);
    std::optional<int> find(const std::string& word) const;
    // Up to `limit` (display name, id) pairs whose name starts with `prefix`, alphabetical.
    std::vector<std::pair<std::string, int>> autocomplete(const std::string& prefix, size_t limit) const;
    size_t size() const { return size_; }

private:
    struct Node;
    std::unique_ptr<Node> root_;
    size_t size_ = 0;

    const Node* locate(const std::string& key) const;
    static void collect(const Node* n, std::vector<std::pair<std::string, int>>& out, size_t limit);
};

}  // namespace bvs
