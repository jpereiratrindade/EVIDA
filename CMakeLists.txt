cmake_minimum_required(VERSION 3.25)
project(evida VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Python3 COMPONENTS Interpreter)
find_path(BOOST_INCLUDE_DIR NAMES boost/beast.hpp REQUIRED)

set(EVIDA_SQLITE_TARGET SQLite3::SQLite3)

add_library(evida-core STATIC
    src/core/evidence.cpp
    src/core/store.cpp
    src/core/auth.cpp
    src/core/ingest.cpp)
target_include_directories(evida-core PUBLIC include)
target_link_libraries(evida-core PUBLIC ${EVIDA_SQLITE_TARGET} OpenSSL::Crypto OpenSSL::SSL)
target_compile_options(evida-core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(evida-core PRIVATE EVIDA_MIGRATIONS_DIR="${CMAKE_SOURCE_DIR}/storage/migrations")

add_executable(evida-server src/server/main.cpp)
target_include_directories(evida-server PRIVATE ${BOOST_INCLUDE_DIR})
target_link_libraries(evida-server PRIVATE evida-core Threads::Threads)
target_compile_definitions(evida-server PRIVATE
    BOOST_ERROR_CODE_HEADER_ONLY BOOST_SYSTEM_NO_DEPRECATED
    EVIDA_WEB_ROOT="${CMAKE_SOURCE_DIR}/web")
target_compile_options(evida-server PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(evida-core-tests tests/core_tests.cpp)
  target_link_libraries(evida-core-tests PRIVATE evida-core)
  target_compile_options(evida-core-tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME evida.core.provenance COMMAND evida-core-tests)
  if(Python3_Interpreter_FOUND)
    add_test(NAME evida.http.smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tests/http_smoke.py" "$<TARGET_FILE:evida-server>")
    set_tests_properties(evida.http.smoke PROPERTIES TIMEOUT 30)
  endif()
endif()
