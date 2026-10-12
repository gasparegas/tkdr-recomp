cmake_minimum_required(VERSION 3.20)
project(tkdr_recomp C CXX)
set(CMAKE_CXX_STANDARD 17)

# RECOMP_DYNAMIC (web only): a build with no game code or data inside, which can
# be hosted. The page recompiles the player's own EBOOT in the browser (web/recomp).
option(RECOMP_DYNAMIC "Hostable web build: recompile the player's EBOOT in the browser" OFF)

# Output of `python3 -m recomp.emit <elf> <dir>` (contains game-derived code: never commit)
set(GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen" CACHE PATH "Generated sources")
if(NOT RECOMP_DYNAMIC)
    include(${GEN_DIR}/sources.cmake)
endif()

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
if(RECOMP_DYNAMIC)
    target_compile_definitions(tkdr PRIVATE RECOMP_DYNAMIC)
    target_include_directories(tkdr PRIVATE runtime)
else()
    target_include_directories(tkdr PRIVATE runtime ${GEN_DIR})
endif()
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
    # the page knows whether it has to recompile the player's EBOOT
    if(RECOMP_DYNAMIC)
        set(TKDR_DYNAMIC true)
    else()
        set(TKDR_DYNAMIC false)
    endif()
    configure_file(runtime/web/shell.html ${CMAKE_BINARY_DIR}/shell.html @ONLY)
    # Browser build: pthreads = Web Workers on a shared heap (needs COOP/COEP headers)
    target_compile_options(tkdr PRIVATE -pthread -sUSE_SDL=2)
    target_link_options(tkdr PRIVATE
        -pthread -sUSE_SDL=2
        -sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=768MB
        -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB
        -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS=callMain,ENV,FS,IDBFS,addRunDependency,removeRunDependency -sEXPORTED_FUNCTIONS=_main,_web_present,_web_frames,_web_time,_web_resize
        -sENVIRONMENT=web,worker -lidbfs.js -sFORCE_FILESYSTEM=1
        --shell-file ${CMAKE_BINARY_DIR}/shell.html)
    if(RECOMP_DYNAMIC)
        # the generated module calls these; the page writes /game before main()
        target_link_options(tkdr PRIVATE
            --js-library ${CMAKE_SOURCE_DIR}/runtime/web/game_lib.js
            -sEXPORTED_FUNCTIONS=_main,_web_present,_web_frames,_web_time,_web_resize,_hle_call,_vfpu_exec,_recomp_trap_code,_recomp_call_miss,_game_code_ptr,_game_code_len,_game_gmem
            -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$tkShareGameModule)
        # the in-browser recompiler and the NID names are served next to the page (copied on every build)
        add_custom_target(web_files ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web/recomp ${CMAKE_BINARY_DIR}/recomp
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/recomp/nids.json ${CMAKE_BINARY_DIR}/recomp/nids.json
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/web/_headers ${CMAKE_BINARY_DIR}/_headers)
    else()
        target_link_options(tkdr PRIVATE --preload-file ${GEN_DIR}/image.bin@image.bin)
    endif()
    set_target_properties(tkdr PROPERTIES SUFFIX ".html"  # relink when the page or the JS glue changes
                          LINK_DEPENDS "${CMAKE_BINARY_DIR}/shell.html;${CMAKE_SOURCE_DIR}/runtime/web/game_lib.js")
    target_link_libraries(tkdr PRIVATE m)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(tkdr PRIVATE m Threads::Threads SDL2::SDL2)
endif()
