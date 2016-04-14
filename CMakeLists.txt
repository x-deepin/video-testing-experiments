cmake_minimum_required(VERSION 2.8)

project(video-check-scripts)

add_compile_options(-std=c++11)

option(USE_CLANG "use clang++ to build" OFF)

if (USE_CLANG)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig)
pkg_check_modules(DEP_LIBS REQUIRED glib-2.0 gobject-2.0 x11 gl xcb glew cogl-1.0)

add_compile_options(${DEP_LIBS_CFLAGS})
include_directories(${DEP_LIBS_INCLUDE_DIRS})

set(TARGETS opengl_test cogl_test)

foreach(target ${TARGETS})
add_executable(${target} ${target}.cpp)
target_link_libraries(${target} ${DEP_LIBS_LIBRARIES})
endforeach()

install(TARGETS ${TARGETS} DESTINATION bin)


