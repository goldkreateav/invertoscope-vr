cmake_minimum_required(VERSION 3.22.1)
project(openxr_bootstrap)

add_library(openxr_bootstrap SHARED openxr_bootstrap.cpp)

find_library(log-lib log)
find_library(android-lib android)
find_library(dl-lib dl)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)

target_link_libraries(
        openxr_bootstrap
        ${log-lib}
        ${android-lib}
        ${dl-lib}
        ${egl-lib}
        ${gles-lib}
)
