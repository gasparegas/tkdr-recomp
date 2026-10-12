cmake_minimum_required(VERSION 3.20)
project(tkdr_recomp C CXX)
set(CMAKE_CXX_STANDARD 17)

# Output of `python3 -m recomp.emit <elf> <dir>` (contains game-derived code: never commit)
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Generated sources")
include(${GEN_DIR}/sources.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB HLE_SOURCES runtime/hle/*.c runtime/hle/*.cpp runtime/gpu/*.c runtime/ui/*.cpp)
# Dear ImGui (MIT) for the settings overlay
set(IMGUI third_party/imgui)
add_library(imgui STATIC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
            ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_sdl2.cpp
            ${IMGUI}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui PUBLIC ${IMGUI})

# ATRAC3/ATRAC3+ decoders from FFmpeg (LGPL-2.1+), see third_party/at3_standalone
file(GLOB AT3_SOURCES third_party/at3_standalone/*.cpp)
add_library(at3 STATIC ${AT3_SOURCES})
target_include_directories(at3 PUBLIC third_party/at3_standalone PRIVATE third_party/at3_standalone/shim)
set_target_properties(at3 PROPERTIES CXX_STANDARD 17)
if(EMSCRIPTEN)
    target_compile_options(at3 PRIVATE -pthread)
    target_compile_options(imgui PRIVATE -pthread -sUSE_SDL=2)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(imgui PUBLIC SDL2::SDL2)
endif()
add_executable(tkdr runtime/main.c runtime/vfpu.c runtime/frontend.c runtime/web/web.c ${HLE_SOURCES} ${GENERATED_SOURCES})
target_include_directories(tkdr PRIVATE runtime ${GEN_DIR})
target_link_libraries(tkdr PRIVATE at3 imgui)
# minimal FFmpeg (H.264 only), built by tools/build_ffmpeg.sh
if(EMSCRIPTEN)
    set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/web)
else()
    set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/native)
endif()
target_include_directories(tkdr PRIVATE ${FFMPEG_DIR}/include)
target_link_libraries(tkdr PRIVATE ${FFMPEG_DIR}/lib/libavcodec.a ${FFMPEG_DIR}/lib/libavutil.a)
# generated code relies on wrapping arithmetic and unaligned memcpy
target_compile_options(tkdr PRIVATE -fwrapv -fno-strict-aliasing -Wno-unused-label
                       -Wno-unused-variable -Wno-parentheses-equality -Wno-tautological-compare)
if(EMSCRIPTEN)
    # Browser build: pthreads = Web Workers on a shared heap (needs COOP/COEP headers)
    target_compile_options(tkdr PRIVATE -pthread -sUSE_SDL=2)
    target_link_options(tkdr PRIVATE
        -pthread -sUSE_SDL=2
        -sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=768MB
        -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB
        -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=callMain,ENV,FS,IDBFS,addRunDependency,removeRunDependency -sEXPORTED_FUNCTIONS=_main,_web_present,_web_frames,_web_time
        -sENVIRONMENT=web,worker -lidbfs.js -sFORCE_FILESYSTEM=1
        --preload-file ${GEN_DIR}/image.bin@image.bin
        --shell-file ${CMAKE_SOURCE_DIR}/runtime/web/shell.html)
    set_target_properties(tkdr PROPERTIES SUFFIX ".html")
    target_link_libraries(tkdr PRIVATE m)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(tkdr PRIVATE m Threads::Threads SDL2::SDL2)
endif()
