cmake_minimum_required(VERSION 3.28)
project(h265sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.92.9b GIT_SHALLOW TRUE)
FetchContent_Declare(implot GIT_REPOSITORY https://github.com/epezent/implot.git GIT_TAG v1.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui implot)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${implot_SOURCE_DIR})
target_link_libraries(imgui PUBLIC SDL3::SDL3)

set(H265_WARNINGS -Wall -Wextra -Werror=switch)

# Pure code: cores, interpreters, simulator. No FFmpeg, Asio or SDL, so it
# cannot do I/O by construction.
add_library(h265core STATIC
    client/core.cpp
    client/run.cpp
    common/config.cpp
    common/log.cpp
    common/nal.cpp
    common/wire.cpp
    server/core.cpp
    server/run.cpp
    server/source.cpp
    sim/sim.cpp
)
target_include_directories(h265core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(h265core PRIVATE ${H265_WARNINGS})

# libavcodec-backed Encoder/Decoder ports.
add_library(h265av STATIC
    server/encoder_av.cpp
    client/decoder_av.cpp
)
target_link_libraries(h265av PUBLIC h265core PkgConfig::FFMPEG Threads::Threads)
target_compile_options(h265av PRIVATE ${H265_WARNINGS})

# Real runtime ports shared by both binaries (clock, Asio timers, CSV, control TCP).
add_library(h265rt STATIC common/real.cpp)
target_include_directories(h265rt PUBLIC ${ASIO_INCLUDE_DIR})
target_compile_definitions(h265rt PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(h265rt PUBLIC h265core Threads::Threads)
target_compile_options(h265rt PRIVATE ${H265_WARNINGS})

add_executable(h265sim-server server/main.cpp)
target_link_libraries(h265sim-server PRIVATE h265av h265rt)
target_compile_options(h265sim-server PRIVATE ${H265_WARNINGS})

add_executable(h265sim-client client/main.cpp client/ui.cpp)
target_link_libraries(h265sim-client PRIVATE h265av h265rt imgui)
target_compile_options(h265sim-client PRIVATE ${H265_WARNINGS})

enable_testing()
include(GoogleTest)
add_executable(h265sim-tests
    tests/test_client.cpp
    tests/test_codec.cpp
    tests/test_common.cpp
    tests/test_server.cpp
    tests/test_sim.cpp
    tests/test_source.cpp
)
target_link_libraries(h265sim-tests PRIVATE h265core h265av GTest::gtest_main)
target_compile_definitions(h265sim-tests PRIVATE H265_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(h265sim-tests PRIVATE ${H265_WARNINGS})
gtest_discover_tests(h265sim-tests)
