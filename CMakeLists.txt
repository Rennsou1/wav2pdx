cmake_minimum_required(VERSION 3.10)

project(wav2pdx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-Wall -O3")
set(CMAKE_CXX_FLAGS_DEBUG "-Wall -g")

add_executable(wav2pdx
    src/wav2pdx.cpp
)
