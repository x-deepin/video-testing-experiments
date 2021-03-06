cmake_minimum_required(VERSION 2.8)

project(video-check-scripts)


option(USE_CLANG "use clang++ to build" OFF)

if (USE_CLANG)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig)
pkg_check_modules(DEP_LIBS REQUIRED glib-2.0 gobject-2.0
    x11 gl xcb glew cogl-1.0
    gbm libdrm libdrm_amdgpu libdrm_intel libdrm_nouveau libdrm_radeon
    egl glesv2 xrandr xcomposite xdamage)

add_compile_options(${DEP_LIBS_CFLAGS})
include_directories(${DEP_LIBS_INCLUDE_DIRS})

set(TARGETS opengl_test cogl_test xorg_test)

foreach(target ${TARGETS})
    add_executable(${target} ${target}.cpp glutil.cc)
    target_compile_options(${target} PRIVATE -std=c++11)
    target_link_libraries(${target} ${DEP_LIBS_LIBRARIES})
endforeach()

add_executable(drm_test drm_test.c)
target_link_libraries(drm_test ${DEP_LIBS_LIBRARIES})

install(TARGETS ${TARGETS} DESTINATION bin)


