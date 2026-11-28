cmake_minimum_required(VERSION 3.24)
project(WhackedRecomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ----------------------------------------------------------------------------
# External dependencies: SDL2 (static on Win32, pkg-config on Linux), ImGui, stb
# ----------------------------------------------------------------------------
if(WIN32)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SDL_LIBC   ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-2.30.9
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(SDL2)
    set(WHACKED_SDL_TARGET SDL2::SDL2-static)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    find_package(OpenGL REQUIRED)
    find_package(Threads REQUIRED)
    set(WHACKED_SDL_TARGET PkgConfig::SDL2)
endif()

find_package(Git QUIET)
if(GIT_FOUND)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.9b
        GIT_SHALLOW    TRUE
    )
else()
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()
FetchContent_MakeAvailable(imgui)

set(STB_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/stb_single")
if(NOT EXISTS "${STB_INCLUDE_DIR}/stb_image_write.h")
    file(MAKE_DIRECTORY "${STB_INCLUDE_DIR}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h"
        "${STB_INCLUDE_DIR}/stb_image_write.h"
        SHOW_PROGRESS
        STATUS STB_DL_STATUS
    )
    list(GET STB_DL_STATUS 0 STB_DL_OK)
    if(NOT STB_DL_OK EQUAL 0)
        message(FATAL_ERROR "Failed to download stb_image_write.h: ${STB_DL_STATUS}")
    endif()
endif()
if(NOT EXISTS "${STB_INCLUDE_DIR}/stb_image.h")
    file(MAKE_DIRECTORY "${STB_INCLUDE_DIR}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${STB_INCLUDE_DIR}/stb_image.h"
        SHOW_PROGRESS
        STATUS STB_IMG_DL_STATUS
    )
    list(GET STB_IMG_DL_STATUS 0 STB_IMG_DL_OK)
    if(NOT STB_IMG_DL_OK EQUAL 0)
        message(FATAL_ERROR "Failed to download stb_image.h: ${STB_IMG_DL_STATUS}")
    endif()
endif()

# Dear ImGui static library
set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp"
)
if(WIN32)
    list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
endif()

add_library(whacked_imgui STATIC ${IMGUI_SOURCES})
target_include_directories(whacked_imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)
target_link_libraries(whacked_imgui PUBLIC ${WHACKED_SDL_TARGET})

# ----------------------------------------------------------------------------
# WhackedRecomp Executable
# ----------------------------------------------------------------------------
if(MSVC)
    add_compile_options(/MP /W2 /wd4100 /wd4101 /wd4102 /wd4189 /wd4244 /wd4267 /wd4305 /wd4996 /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-sign-compare -Wno-switch)
endif()

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/*.cpp")

set(RUNTIME_SOURCES
    src/main.cpp
    src/runtime/app_config.cpp
    src/runtime/xbox_mem.cpp
    src/runtime/xiso_vfs.cpp
    src/runtime/hle_kernel.cpp
    src/runtime/hle_xapi_crt.cpp
    src/runtime/hle_input.cpp
    src/runtime/hle_audio.cpp
    src/runtime/hle_bink.cpp
    src/runtime/hle_net.cpp
    src/runtime/hle_d3d8.cpp
    src/runtime/nv2a_shaders.cpp
    src/runtime/gfx_dispatch.cpp
    src/runtime/gfx_opengl.cpp
    src/runtime/launcher_ui.cpp
)
if(WIN32)
    list(APPEND RUNTIME_SOURCES src/runtime/d3d11_renderer.cpp)
endif()

if(WIN32)
    add_executable(WhackedRecomp WIN32 ${RUNTIME_SOURCES} ${RECOMP_SOURCES} src/whacked.rc)
else()
    add_executable(WhackedRecomp ${RUNTIME_SOURCES} ${RECOMP_SOURCES})
endif()

if(MSVC)
    set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS "/O2;/Oi")
    # Use /O1 /Ob1 /Oi on generated 45k-line basic-block recomp units (fast compile + optimized native code)
    set_source_files_properties(${RECOMP_SOURCES}  PROPERTIES COMPILE_OPTIONS "/O1;/Ob1;/Oi")
else()
    target_compile_options(WhackedRecomp PRIVATE -O2 -fno-strict-aliasing -fwrapv)
    target_link_options(WhackedRecomp PRIVATE -rdynamic "-Wl,-rpath,$ORIGIN/lib:$ORIGIN/../lib")
endif()

set(RL_ACH_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/rl_achievements)

target_include_directories(WhackedRecomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rl_achievements/include"
    "${STB_INCLUDE_DIR}"
)

target_link_libraries(WhackedRecomp PRIVATE
    whacked_imgui
    ${WHACKED_SDL_TARGET}
    rl_achievements
)

if(WIN32)
    target_link_libraries(WhackedRecomp PRIVATE
        d3d11
        dxgi
        d3dcompiler
        opengl32
        ws2_32
        winmm
        user32
        gdi32
        ole32
        shell32
        comdlg32
        dwmapi
        shlwapi
    )
else()
    target_link_libraries(WhackedRecomp PRIVATE
        OpenGL::GL
        Threads::Threads
        dl
        m
    )
endif()

if(WIN32)
    set(dxvk_archive "${CMAKE_BINARY_DIR}/dxvk-2.7.1.tar.gz")
    set(dxvk_hash "d85ce7c79f57ecd765aaa1b9e7007cb875e6fde9f6d331df799bce73d513ce87")
    if(NOT EXISTS "${dxvk_archive}")
        file(DOWNLOAD "https://github.com/doitsujin/dxvk/releases/download/v2.7.1/dxvk-2.7.1.tar.gz"
             "${dxvk_archive}" EXPECTED_HASH SHA256=${dxvk_hash})
    endif()
    file(SHA256 "${dxvk_archive}" actual_dxvk_hash)
    if(NOT actual_dxvk_hash STREQUAL dxvk_hash)
        message(FATAL_ERROR "DXVK archive checksum mismatch")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${dxvk_archive}" DESTINATION "${CMAKE_BINARY_DIR}/_deps"
         PATTERNS "dxvk-2.7.1/x64/d3d11.dll" "dxvk-2.7.1/x64/dxgi.dll")
    add_custom_command(TARGET WhackedRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/_deps/dxvk-2.7.1/x64" "$<TARGET_FILE_DIR:WhackedRecomp>/dxvk")
endif()

enable_testing()
add_executable(whacked_tests
    tests/whacked_tests.cpp
    src/runtime/xbox_mem.cpp
    src/runtime/xiso_vfs.cpp
    src/runtime/app_config.cpp
    src/runtime/hle_kernel.cpp
    src/runtime/hle_input.cpp
    src/runtime/hle_audio.cpp
    src/runtime/hle_net.cpp
)
target_include_directories(whacked_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rl_achievements/include"
)
target_link_libraries(whacked_tests PRIVATE ${WHACKED_SDL_TARGET} rl_achievements)
target_compile_definitions(whacked_tests PRIVATE WHACKED_RUNTIME_TESTS)
if(WIN32)
    target_link_libraries(whacked_tests PRIVATE ws2_32 user32 winmm dbghelp)
else()
    target_link_libraries(whacked_tests PRIVATE Threads::Threads dl m)
endif()
if(MSVC)
    target_compile_options(whacked_tests PRIVATE /O2)
else()
    target_compile_options(whacked_tests PRIVATE -O2 -fno-strict-aliasing -fwrapv)
endif()
add_test(NAME WhackedRuntimeTests COMMAND whacked_tests)
