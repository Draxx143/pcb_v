cmake_minimum_required(VERSION 3.16)
project(PcbAiViewer VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PCBV_BUILD_GUI "Build the Qt6 desktop GUI (needs qt6-base-dev)" ON)
option(PCBV_BUILD_TESTS "Build the parser smoke test" ON)

# ---------------------------------------------------------------------------
# Core: Altium OLE2 + PcbDoc/SchDoc parser (no third-party dependencies)
# ---------------------------------------------------------------------------
add_library(pcbv_core STATIC
    src/core/CompoundFile.cpp
    src/core/CompoundFile.h
    src/core/AltiumText.cpp
    src/core/AltiumText.h
    src/core/AltiumInternal.h
    src/core/AltiumParser.cpp
    src/core/AltiumParser.h
    src/core/AsciiParser.cpp
    src/core/BoardRender.cpp
    src/core/BoardRender.h
    src/core/BoardSvg.cpp
    src/core/LayerStyle.h
    src/core/Export.cpp
    src/core/Export.h
    src/core/Model.h
)
target_include_directories(pcbv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(pcbv_core PRIVATE /W4)
else()
    target_compile_options(pcbv_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Command line tool (always built)
# ---------------------------------------------------------------------------
add_executable(pcbv-cli src/cli/main.cpp)
target_link_libraries(pcbv-cli PRIVATE pcbv_core)

# ---------------------------------------------------------------------------
# Qt6 GUI (optional)
# ---------------------------------------------------------------------------
if(PCBV_BUILD_GUI)
    find_package(Qt6 6.2 QUIET COMPONENTS Core Gui Widgets)
    if(Qt6_FOUND)
        set(CMAKE_AUTOMOC ON)
        add_executable(pcbv-gui
            src/gui/main.cpp
            src/gui/BoardView.cpp
            src/gui/BoardView.h
            src/gui/MainWindow.cpp
            src/gui/MainWindow.h
        )
        target_link_libraries(pcbv-gui PRIVATE pcbv_core Qt6::Core Qt6::Gui Qt6::Widgets)
        message(STATUS "Qt ${Qt6_VERSION} found — building pcbv-gui")
    else()
        message(WARNING
            "Qt6 (Core/Gui/Widgets) was not found — only the command line tool 'pcbv-cli' will be built.\n"
            "  Debian/Ubuntu : sudo apt install qt6-base-dev\n"
            "  Fedora        : sudo dnf install qt6-qtbase-devel\n"
            "  Arch          : sudo pacman -S qt6-base\n"
            "Then re-run cmake.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PCBV_BUILD_TESTS)
    enable_testing()
    add_executable(pcbv-smoke tests/smoke.cpp)
    target_link_libraries(pcbv-smoke PRIVATE pcbv_core)
    add_test(NAME parser_smoke COMMAND pcbv-smoke --self-test)

    # Run the smoke test against real files:  ctest --test-dir build -V
    # or:  ./build/pcbv-smoke path/to/file.PcbDoc path/to/file.SchDoc
endif()

install(TARGETS pcbv-cli RUNTIME DESTINATION bin)
if(TARGET pcbv-gui)
    install(TARGETS pcbv-gui RUNTIME DESTINATION bin)
endif()
