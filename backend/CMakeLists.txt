cmake_minimum_required(VERSION 3.16)
project(BiometricVotingBackend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.3)
FetchContent_MakeAvailable(httplib)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(voting_backend
  main.cpp
  API/AuthController.cpp
  API/VoteController.cpp
  API/ResultController.cpp
  API/SqlController.cpp
  API/EnrollController.cpp
  Services/EnrollmentService.cpp
  Tools/DemoSeeder.cpp
  DataStructures/Trie.cpp
  DataStructures/MaxHeap.cpp
  DataStructures/SegmentTree.cpp
  DataStructures/SkipList.cpp
  Database/DatabaseManager.cpp
  Services/AuthService.cpp
  Services/VotingService.cpp
  Services/ResultService.cpp
  Hardware/SerialListener.cpp)

target_include_directories(voting_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voting_backend PRIVATE httplib::httplib SQLite::SQLite3 Threads::Threads)
target_compile_definitions(voting_backend PRIVATE
  DEFAULT_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/Database/schema.sql"
  DEFAULT_DB_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/voting.db"
  DEFAULT_FRONTEND_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../frontend")
if(WIN32)
  target_compile_definitions(voting_backend PRIVATE _WIN32_WINNT=0x0A00)
endif()
if(NOT MSVC)
  target_compile_options(voting_backend PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- optional: from-scratch data-structure + SHA-256 tests (ctest) ----
option(BVS_BUILD_TESTS "Build core_tests" ON)
if(BVS_BUILD_TESTS)
  enable_testing()
  add_executable(core_tests tests/test_core.cpp
    DataStructures/Trie.cpp DataStructures/MaxHeap.cpp DataStructures/SegmentTree.cpp DataStructures/SkipList.cpp)
  target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(core_tests PRIVATE Threads::Threads)
  add_test(NAME core_tests COMMAND core_tests)
endif()
