cmake_minimum_required(VERSION 3.20)
project(trpg_cpp VERSION 44.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(CRYPT_LIBRARY crypt REQUIRED)

add_executable(trpg_cpp
  src/main.cpp
  src/Config.cpp
  src/Security.cpp
  src/CoreService.cpp
  src/LegacyCompat.cpp
  src/RoomSocket.cpp
)
target_include_directories(trpg_cpp PRIVATE include)
target_link_libraries(trpg_cpp PRIVATE Drogon::Drogon OpenSSL::Crypto ${CRYPT_LIBRARY})
target_compile_options(trpg_cpp PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(security_test tests/security_test.cpp src/Security.cpp)
  target_include_directories(security_test PRIVATE include)
  target_link_libraries(security_test PRIVATE OpenSSL::Crypto ${CRYPT_LIBRARY})
  target_compile_options(security_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME security_test COMMAND security_test)
endif()
