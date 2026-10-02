cmake_minimum_required(VERSION 3.16)
project(calculinux-emulator-launcher VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(LAUNCHER_BUILD_GUI "Build the window (needs SDL2); otherwise only the headless commands" ON)
option(LAUNCHER_BUILD_TESTS "Build the tests" ON)
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui" CACHE PATH "Dear ImGui sources")

include(GNUInstallDirs)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Everything but the window: index, state directory, downloads.
add_library(launcher_core STATIC
    src/core/catalog.cpp
    src/core/download.cpp
    src/core/http.cpp
    src/core/index.cpp
    src/core/session.cpp
    src/core/sha256.cpp
    src/core/state.cpp
    src/core/util.cpp
)
target_include_directories(launcher_core PUBLIC src)
target_include_directories(launcher_core SYSTEM PUBLIC third_party)
target_link_libraries(launcher_core PUBLIC CURL::libcurl Threads::Threads)
target_compile_options(launcher_core PRIVATE -Wall -Wextra)

add_executable(calculinux-emulator-launcher src/main.cpp src/cli.cpp)
target_compile_definitions(calculinux-emulator-launcher PRIVATE LAUNCHER_VERSION="${PROJECT_VERSION}")
target_compile_options(calculinux-emulator-launcher PRIVATE -Wall -Wextra)
target_link_libraries(calculinux-emulator-launcher PRIVATE launcher_core)

if(LAUNCHER_BUILD_GUI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_link_libraries(imgui PUBLIC PkgConfig::SDL2)
    # The demo and debug windows are not used.
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)

    target_sources(calculinux-emulator-launcher PRIVATE src/gui.cpp)
    target_link_libraries(calculinux-emulator-launcher PRIVATE imgui)
else()
    target_sources(calculinux-emulator-launcher PRIVATE src/gui_stub.cpp)
endif()

install(TARGETS calculinux-emulator-launcher RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LAUNCHER_BUILD_TESTS)
    enable_testing()
    add_executable(test_core tests/test_core.cpp)
    target_link_libraries(test_core PRIVATE launcher_core)
    add_test(NAME core COMMAND test_core)
    add_test(NAME integration
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE:calculinux-emulator-launcher>)
endif()
