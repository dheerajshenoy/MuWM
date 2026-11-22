cmake_minimum_required(VERSION 3.16)
project(muwm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    wayland-server
    wlroots-0.20
    xkbcommon
)

pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Generate xdg-shell-protocol.h (wlroots headers include it, but don't ship it)
set(XDG_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(XDG_HDR ${CMAKE_BINARY_DIR}/generated/xdg-shell-protocol.h)
add_custom_command(
    OUTPUT ${XDG_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${WAYLAND_SCANNER} server-header ${XDG_XML} ${XDG_HDR}
    DEPENDS ${XDG_XML}
)

add_custom_target(protocols DEPENDS ${XDG_HDR})


add_executable(${PROJECT_NAME}
    src/main.cpp
    src/MuWM.cpp
    src/Output.cpp
    src/Toplevel.cpp

    include/Output.hpp
    include/Toplevel.hpp
    include/wlroots.hpp
    include/MuWM.hpp)

add_dependencies(${PROJECT_NAME} protocols)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_BINARY_DIR}/generated)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::DEPS)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    PROJECT_NAME="${PROJECT_NAME}"
    PROJECT_VERSION="${PROJECT_VERSION}"
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra
)
