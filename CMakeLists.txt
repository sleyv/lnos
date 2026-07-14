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
        liblnos/src/protocol.cpp
        liblnos/src/crypto.cpp
)

add_library(lnos::lnos ALIAS lnos)

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
# lnosctl
#
add_executable(lnosctl
        lnosctl/src/main.cpp
)

target_link_libraries(lnosctl
        PRIVATE
        lnos::lnos
)