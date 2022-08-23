cmake_minimum_required(VERSION 3.1.0 FATAL_ERROR)
project(makekey)

find_package(Qt5Core REQUIRED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../qtnetworkng/ qtnetworkng)

add_executable(makekey main.cpp)
target_link_libraries(makekey Qt5::Core qtnetworkng)
