cmake_minimum_required(VERSION 3.20)
project(trpg_cpp VERSION 62.23.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(CRYPT_LIBRARY crypt REQUIRED)

# 標準目錄為主；若 GitHub 網頁誤把資料夾攤平，仍保留相容編譯模式。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
  set(TRPG_SRC_PREFIX "src/")
  set(TRPG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
  set(TRPG_TEST_SOURCE "tests/security_test.cpp")
else()
  set(TRPG_SRC_PREFIX "")
  set(TRPG_GENERATED_INCLUDE "${CMAKE_BINARY_DIR}/flat_include")
  file(MAKE_DIRECTORY "${TRPG_GENERATED_INCLUDE}/trpg")
  foreach(header Config.h CoreService.h LegacyCompat.h LegacyRouteManifest.h RoomSocket.h Security.h WorldSystems.h RuleEngine.h RuleCombatBridge.h GearAffixSystem.h EventSkillSystem.h)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
      message(FATAL_ERROR "Missing flat-layout header: ${header}")
    endif()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${header}" "${TRPG_GENERATED_INCLUDE}/trpg/${header}" COPYONLY)
  endforeach()
  set(TRPG_INCLUDE_DIR "${TRPG_GENERATED_INCLUDE}")
  set(TRPG_TEST_SOURCE "security_test.cpp")
endif()

add_executable(trpg_cpp
  ${TRPG_SRC_PREFIX}main.cpp
  ${TRPG_SRC_PREFIX}Config.cpp
  ${TRPG_SRC_PREFIX}Security.cpp
  ${TRPG_SRC_PREFIX}CoreService.cpp
  ${TRPG_SRC_PREFIX}LegacyCompat.cpp
  ${TRPG_SRC_PREFIX}RoomSocket.cpp
  ${TRPG_SRC_PREFIX}WorldSystems.cpp
  ${TRPG_SRC_PREFIX}RuleEngine.cpp
  ${TRPG_SRC_PREFIX}RuleCombatBridge.cpp
  ${TRPG_SRC_PREFIX}GearAffixSystem.cpp
  ${TRPG_SRC_PREFIX}EventSkillSystem.cpp
)
target_include_directories(trpg_cpp PRIVATE "${TRPG_INCLUDE_DIR}")
target_link_libraries(trpg_cpp PRIVATE Drogon::Drogon OpenSSL::Crypto ${CRYPT_LIBRARY})
target_compile_options(trpg_cpp PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(security_test ${TRPG_TEST_SOURCE} ${TRPG_SRC_PREFIX}Security.cpp)
  target_include_directories(security_test PRIVATE "${TRPG_INCLUDE_DIR}")
  target_link_libraries(security_test PRIVATE OpenSSL::Crypto ${CRYPT_LIBRARY})
  target_compile_options(security_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME security_test COMMAND security_test)
endif()
