cmake_minimum_required(VERSION 3.16)

project(lnos)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)


#
# liblnos
#
add_library(lnos
        liblnos/src/config.cpp
        liblnos/src/crypto.cpp
)
set_property(TARGET lnos PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(lnos::lnos ALIAS lnos)

#
# libnss_lnos
#
add_library(nss_lnos SHARED
        liblnos/src/nss_lnos.cpp
)
set_target_properties(nss_lnos PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "nss_lnos"
        VERSION "2"
)
target_link_libraries(nss_lnos
        PRIVATE
        lnos::lnos
)

target_include_directories(lnos
        PUBLIC
        liblnos/include
        ${SODIUM_INCLUDE_DIRS}
)

target_link_libraries(lnos
        PUBLIC
        ${SODIUM_LIBRARIES}
)


#
# lnosd
#
add_executable(lnosd
        lnosd/src/main.cpp
        lnosd/src/registry.cpp
)

target_include_directories(lnosd
        PRIVATE
        lnosd/include
)

target_link_libraries(lnosd
        PRIVATE
        lnos::lnos
)


#
# lnos_tests
#
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

add_executable(lnos_tests
        tests/test_lnos.cpp
)

target_link_libraries(lnos_tests
        PRIVATE
        lnos::lnos
        gtest
        gtest_main
)

enable_testing()
add_test(NAME lnos_unit_tests COMMAND lnos_tests)


#
# lnosctl
#
add_executable(lnosctl
        lnosctl/src/main.cpp
)

target_link_libraries(lnosctl
        PRIVATE
        lnos::lnos
)
