#pragma once
#include "../Database/DatabaseManager.h"

namespace bvs {

// Offline demo-data generator (flag --simulate N). Creates constituencies, parties, candidates,
// N voters and ~75% turnout spread over the last 8 hours, writing a VALID hash chain through
// the same commitVote() transaction the live path uses. Refuses to touch a DB that already
// contains voters or votes, so it can never contaminate a real election.
void seedDemo(DatabaseManager& db, int voterCount);

}  // namespace bvs
