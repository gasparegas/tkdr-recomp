cmake_minimum_required(VERSION 3.20)
project(tkdr_recomp C)

# Output of `python3 -m recomp.emit <elf> <dir>` (contains game-derived code: never commit)
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Generated sources")
include(${GEN_DIR}/sources.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB HLE_SOURCES runtime/hle/*.c runtime/gpu/*.c)
add_executable(tkdr runtime/main.c runtime/vfpu.c runtime/frontend.c ${HLE_SOURCES} ${GENERATED_SOURCES})
target_include_directories(tkdr PRIVATE runtime ${GEN_DIR})
# generated code relies on wrapping arithmetic and unaligned memcpy
target_compile_options(tkdr PRIVATE -fwrapv -fno-strict-aliasing -Wno-unused-label
                       -Wno-unused-variable -Wno-parentheses-equality -Wno-tautological-compare)
if(EMSCRIPTEN)
    # Browser build: pthreads = Web Workers on a shared heap (needs COOP/COEP headers)
    target_sources(tkdr PRIVATE runtime/web/web.c)
    target_compile_options(tkdr PRIVATE -pthread -sUSE_SDL=2)
    target_link_options(tkdr PRIVATE
        -pthread -sUSE_SDL=2
        -sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=768MB
        -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB
        -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=callMain,ENV -sEXPORTED_FUNCTIONS=_main,_web_present,_web_frames,_web_time
        -sENVIRONMENT=web,worker
        --preload-file ${GEN_DIR}/image.bin@image.bin
        --shell-file ${CMAKE_SOURCE_DIR}/runtime/web/shell.html)
    set_target_properties(tkdr PROPERTIES SUFFIX ".html")
    target_link_libraries(tkdr PRIVATE m)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(tkdr PRIVATE m Threads::Threads SDL2::SDL2)
endif()
