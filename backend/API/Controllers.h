#pragma once
#include "../Database/DatabaseManager.h"
#include "../Hardware/SerialListener.h"
#include "../Services/AuthService.h"
#include "../Services/ResultService.h"
#include "../Services/VotingService.h"

#if __has_include("httplib.h")
#include "httplib.h"
#elif __has_include("../httplib.h")
#include "../httplib.h"
#else
#include <httplib.h>
#endif

namespace bvs {

class EnrollmentService;

void registerAuthRoutes(httplib::Server& svr, AuthService& auth, ResultService& results, VotingService& voting,
                        const SerialListener* serial, DatabaseManager* db = nullptr);

void registerVoteRoutes(httplib::Server& svr, AuthService& auth, VotingService& voting, DatabaseManager& db);
void registerResultRoutes(httplib::Server& svr, AuthService& auth, ResultService& results, bool publicLeaderboard);
void registerEnrollRoutes(httplib::Server& svr, AuthService& auth, DatabaseManager& db, EnrollmentService& enroll);
void registerSqlRoutes(httplib::Server& svr, AuthService& auth, DatabaseManager& db, ResultService& results);

}  // namespace bvs