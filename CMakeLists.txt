cmake_minimum_required(VERSION 3.16)
project(camera_lvgl LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build LVGL as a static library from third_party
add_subdirectory(third_party/lvgl)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LIBJPEG REQUIRED IMPORTED_TARGET libjpeg)
find_package(Threads REQUIRED)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(camera_lvgl ${APP_SOURCES})

target_include_directories(camera_lvgl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(camera_lvgl PRIVATE
    lvgl::lvgl
    PkgConfig::SDL2
    PkgConfig::LIBJPEG
    Threads::Threads)

target_compile_options(camera_lvgl PRIVATE -Wall -Wextra)
