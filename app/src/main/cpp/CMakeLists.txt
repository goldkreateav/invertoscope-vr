cmake_minimum_required(VERSION 3.22.1)
project(openxr_bootstrap)

add_library(openxr_bootstrap SHARED openxr_bootstrap.cpp)

if(DEFINED ANDROID_NDK)
    set(OPENXR_INCLUDE_FROM_NDK "${ANDROID_NDK}/sources/third_party/openxr/src/include")
    if(EXISTS "${OPENXR_INCLUDE_FROM_NDK}")
        target_include_directories(openxr_bootstrap PRIVATE "${OPENXR_INCLUDE_FROM_NDK}")
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")
    target_include_directories(openxr_bootstrap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")
endif()

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
