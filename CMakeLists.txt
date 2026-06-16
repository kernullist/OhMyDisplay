# OhMyDisplay - top-level build
# MSVC, C++20, /W4, /permissive-, x64. Two executables: main (asInvoker) + elevated helper.
cmake_minimum_required(VERSION 3.21)
project(OhMyDisplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Shared static core + module sources. Globbed for fast iteration during v0.
file(GLOB_RECURSE OMD_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/keeper/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/doctor/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/shell/*.cpp"
)

add_library(omd_core STATIC ${OMD_CORE_SOURCES})
target_include_directories(omd_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(omd_core PUBLIC
    user32
    gdi32
    shell32
    advapi32
    ole32
    oleaut32
    shlwapi
    wevtapi
    powrprof
    pathcch
    shcore
    dwmapi
    version
)

# Optional WebView2 embedded report (uses the static loader to keep a single exe).
# Enabled automatically when the SDK has been placed under third_party/webview2.
set(OMD_WV2_DIR "${CMAKE_SOURCE_DIR}/third_party/webview2")
if(EXISTS "${OMD_WV2_DIR}/include/WebView2.h")
    message(STATUS "WebView2 SDK found - enabling embedded HTML report")
    target_include_directories(omd_core PUBLIC "${OMD_WV2_DIR}/include")
    target_compile_definitions(omd_core PUBLIC OMD_WITH_WEBVIEW2=1)
    target_link_libraries(omd_core PUBLIC "${OMD_WV2_DIR}/lib/x64/WebView2LoaderStatic.lib")
else()
    message(STATUS "WebView2 SDK not found - report uses native text fallback")
endif()

# Main application: windowed, no console, asInvoker manifest via .rc
add_executable(OhMyDisplay WIN32
    "${CMAKE_SOURCE_DIR}/src/app/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/app/app.rc"
)
target_link_libraries(OhMyDisplay PRIVATE omd_core)
# Disable auto-generated manifest; we embed our own through the .rc/.manifest pair.
target_link_options(OhMyDisplay PRIVATE /MANIFEST:NO)

# Elevated helper: console subsystem, requireAdministrator manifest
add_executable(OhMyDisplay.Elevated
    "${CMAKE_SOURCE_DIR}/src/elevated/helper_main.cpp"
    "${CMAKE_SOURCE_DIR}/src/elevated/helper.rc"
)
target_link_libraries(OhMyDisplay.Elevated PRIVATE omd_core)
target_link_options(OhMyDisplay.Elevated PRIVATE /MANIFEST:NO)

# Test harness (no external framework). asInvoker so it can run from CI.
add_executable(omd_tests "${CMAKE_SOURCE_DIR}/tests/test_main.cpp")
target_link_libraries(omd_tests PRIVATE omd_core)
target_link_options(omd_tests PRIVATE /MANIFEST:NO)
