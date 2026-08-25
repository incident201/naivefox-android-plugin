cmake_minimum_required(VERSION 3.22)
project(naivefox_android_launcher LANGUAGES C)

if(NOT DEFINED NAIVEFOX_PACKAGE_ROOT)
    message(FATAL_ERROR "NAIVEFOX_PACKAGE_ROOT must point to the verified runtime package")
endif()

get_filename_component(NAIVEFOX_PACKAGE_ROOT "${NAIVEFOX_PACKAGE_ROOT}" ABSOLUTE)
set(NAIVEFOX_MANIFEST_PATH "${NAIVEFOX_PACKAGE_ROOT}/manifest.json")
set(NAIVEFOX_API_HEADER "${NAIVEFOX_PACKAGE_ROOT}/include/NaiveFoxAPI.h")
if(NOT EXISTS "${NAIVEFOX_MANIFEST_PATH}" OR NOT EXISTS "${NAIVEFOX_API_HEADER}")
    message(FATAL_ERROR "Verified NaiveFox manifest/header is missing")
endif()

file(READ "${NAIVEFOX_MANIFEST_PATH}" NAIVEFOX_MANIFEST_JSON)
string(JSON NAIVEFOX_RUNTIME_PATH GET "${NAIVEFOX_MANIFEST_JSON}" runtime_path)
if(NAIVEFOX_RUNTIME_PATH STREQUAL "" OR NAIVEFOX_RUNTIME_PATH MATCHES "^/"
        OR NAIVEFOX_RUNTIME_PATH MATCHES "(^|/)\\.\\.(/|$)")
    message(FATAL_ERROR "Unsafe runtime_path in NaiveFox manifest: ${NAIVEFOX_RUNTIME_PATH}")
endif()

add_executable(naivefox_launcher launcher.c)
target_compile_features(naivefox_launcher PRIVATE c_std_11)
target_include_directories(naivefox_launcher PRIVATE "${NAIVEFOX_PACKAGE_ROOT}/include")
target_compile_definitions(naivefox_launcher PRIVATE
    NAIVEFOX_RUNTIME_RELATIVE_PATH="runtime/${NAIVEFOX_RUNTIME_PATH}"
)
target_compile_options(naivefox_launcher PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wformat=2
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -fstack-protector-strong
    -fvisibility=hidden
    -D_FORTIFY_SOURCE=2
)
target_link_options(naivefox_launcher PRIVATE
    -Wl,--no-undefined
    -Wl,-z,relro
    -Wl,-z,now
)

find_package(Threads REQUIRED)
target_link_libraries(naivefox_launcher PRIVATE dl Threads::Threads)
