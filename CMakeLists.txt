cmake_minimum_required(VERSION 3.16)
project(muwm VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Plain `cmake -B build` would otherwise build with no optimization at all.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(WANT_SCENEFX "Build with SceneFX support" OFF)
option(MUWM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    wayland-server
    wlroots-0.20
    xkbcommon
    libinput
)

if (WANT_SCENEFX)
    pkg_check_modules(SCENEFX REQUIRED IMPORTED_TARGET scenefx-0.5)
endif()

find_package(Lua REQUIRED)

# --- Wayland protocols -------------------------------------------------------
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
pkg_check_modules(XCB REQUIRED xcb)
pkg_check_modules(XCB_ICCCM REQUIRED xcb-icccm)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOLS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTOCOLS_GENERATED_DIR})

add_library(muwm_protocols INTERFACE)
target_include_directories(muwm_protocols INTERFACE ${PROTOCOLS_GENERATED_DIR})

# add_protocol(<path to protocol .xml>)
# Generates <xml-basename>-protocol.h, e.g. xdg-shell.xml -> xdg-shell-protocol.h
function(add_protocol XML)
    get_filename_component(name ${XML} NAME_WE)
    set(header ${PROTOCOLS_GENERATED_DIR}/${name}-protocol.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${WAYLAND_SCANNER} server-header ${XML} ${header}
        DEPENDS ${XML}
        VERBATIM
    )
    target_sources(muwm_protocols INTERFACE ${header})
endfunction()

add_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
add_protocol(${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)

# Not part of wayland-protocols; vendored from wlr-protocols.
add_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
add_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml)

# --- muwm ----------------------------------------------------------------------

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/MuWM.cpp
    src/Lua.cpp
    src/Output.cpp
    src/Toplevel.cpp
    src/Keyboard.cpp
    src/Pointer.cpp
    src/LayerShellSurface.cpp
    src/Decoration.cpp
    src/EventBus.cpp
    src/Logging.cpp
    src/Popup.cpp
    src/LockSurface.cpp
    src/XUnmanaged.cpp
    src/IdleInhibitor.cpp
    src/Dialog.cpp
    src/Animation.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${LUA_INCLUDE_DIR}
)

# scenefx replaces wlroots' scene-graph and exports the same wlr_scene_*
# symbols with larger structs. It has to come first in the link line, or
# wlroots' versions win and the two libraries corrupt each other's memory.
if (WANT_SCENEFX)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::SCENEFX)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::DEPS
    ${LUA_LIBRARIES}
    xcb
    xcb-icccm
    muwm_protocols
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PROJECT_NAME="${PROJECT_NAME}"
    PROJECT_VERSION="${PROJECT_VERSION}"
)


if (WANT_SCENEFX)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MUWM_HAS_SCENEFX)
endif()


target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-O2>
)

# Cache the heavy wlroots includes once instead of in every source file.
target_precompile_headers(${PROJECT_NAME} PRIVATE include/wlroots.hpp)

# cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DMUWM_SANITIZE=ON
if(MUWM_SANITIZE)
    target_compile_options(${PROJECT_NAME} PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
endif()

# --- Lua stubs -----------------------------------------------------------------
# lua/mu.lua (LuaLS stubs for editors) is generated from the doc comments in
# src/Lua.cpp. `cmake --build build --target lua-stubs` regenerates it; the
# `lua-stubs` test checks the comments against the code and that the committed
# file is current.

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(LUA_STUBS_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_lua_stub.py)

    add_custom_target(lua-stubs
        COMMAND ${Python3_EXECUTABLE} ${LUA_STUBS_SCRIPT}
        COMMENT "Generating lua/mu.lua"
        VERBATIM
    )

    enable_testing()
    add_test(NAME lua-stubs
        COMMAND ${Python3_EXECUTABLE} ${LUA_STUBS_SCRIPT} --check
    )
else()
    message(STATUS "Python3 not found: lua-stubs target and test disabled")
endif()

# --- Install -------------------------------------------------------------------
# `cmake --install build [--prefix <dir>]`. Honors DESTDIR for packaging.

include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# LuaLS stubs, for pointing an editor's `workspace.library` at.
install(FILES lua/mu.lua
    DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/lua
)

install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

# xdg-desktop-portal reads this by matching XDG_CURRENT_DESKTOP=muwm.
install(FILES data/muwm-portals.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal
)
