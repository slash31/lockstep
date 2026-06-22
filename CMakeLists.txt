cmake_minimum_required(VERSION 3.13)
project(lockstep)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(lockstep PRIVATE "src/")
# Anchored to the SDR++ root (not a relative path) so it resolves correctly even
# when this module is symlinked into misc_modules/ from an external repo.
target_include_directories(lockstep PRIVATE "${CMAKE_SOURCE_DIR}/decoder_modules/radio/src")
