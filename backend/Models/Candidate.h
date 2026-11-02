#pragma once
#include <string>
namespace bvs {
struct Candidate {
    int id = 0;
    std::string name;
    int partyId = 0;           // 0 = independent (NULL in SQL)
};
}  // namespace bvs
