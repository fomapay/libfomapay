cmake_minimum_required(VERSION 3.14)
project(libfomapay VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "SDK C++ officiel de FomaPay")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FOMAPAY_BUILD_TESTS "Compiler les tests" ON)
option(FOMAPAY_BUILD_EXAMPLES "Compiler les exemples" ON)

find_package(CURL REQUIRED)

add_library(fomapay
  src/client.cpp
  src/curl_transport.cpp
  src/json.cpp
  src/webhook.cpp
)
add_library(fomapay::fomapay ALIAS fomapay)
target_include_directories(fomapay
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE src
)
target_link_libraries(fomapay PRIVATE CURL::libcurl)
if(MSVC)
  target_compile_options(fomapay PRIVATE /W4 /utf-8)
else()
  target_compile_options(fomapay PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FOMAPAY_BUILD_EXAMPLES)
  add_executable(fomapay_pos examples/pos.cpp)
  target_link_libraries(fomapay_pos PRIVATE fomapay)
endif()

if(FOMAPAY_BUILD_TESTS)
  enable_testing()
  add_executable(fomapay_tests tests/test_fomapay.cpp)
  target_include_directories(fomapay_tests PRIVATE src)
  target_link_libraries(fomapay_tests PRIVATE fomapay)
  add_test(NAME fomapay_tests COMMAND fomapay_tests)
endif()

include(GNUInstallDirs)
install(TARGETS fomapay EXPORT fomapayTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT fomapayTargets NAMESPACE fomapay:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fomapay FILE fomapayConfig.cmake)
