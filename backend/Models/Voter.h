#pragma once
#include <string>
namespace bvs {
struct Voter {
    int sensorId = 0;          // fingerprint template slot on the R307S
    std::string name;
    bool hasVoted = false;
    int constituencyId = 0;    // 0 = none (NULL in SQL)
};
}  // namespace bvs
