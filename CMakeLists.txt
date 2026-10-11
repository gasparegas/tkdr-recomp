cmake_minimum_required(VERSION 3.20)
project(tkdr_recomp C)

# Output of `python3 -m recomp.emit <elf> <dir>` (contains game-derived code: never commit)
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Generated sources")
include(${GEN_DIR}/sources.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB HLE_SOURCES runtime/hle/*.c runtime/gpu/*.c)
find_package(SDL2 REQUIRED)
add_executable(tkdr runtime/main.c runtime/vfpu.c runtime/frontend.c ${HLE_SOURCES} ${GENERATED_SOURCES})
target_include_directories(tkdr PRIVATE runtime ${GEN_DIR})
# generated code relies on wrapping arithmetic and unaligned memcpy
target_compile_options(tkdr PRIVATE -fwrapv -fno-strict-aliasing -Wno-unused-label
                       -Wno-unused-variable -Wno-parentheses-equality -Wno-tautological-compare)
find_package(Threads REQUIRED)
target_link_libraries(tkdr PRIVATE m Threads::Threads SDL2::SDL2)
