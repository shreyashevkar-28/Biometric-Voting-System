#pragma once
#include <cstdint>
#include <string>
namespace bvs {
// Deliberately has NO voter reference: ballot secrecy.
struct Vote {
    int64_t id = 0;
    std::string candidateName;
    int64_t timestampMs = 0;
    std::string previousHash;
    std::string currentHash;
};
}  // namespace bvs
