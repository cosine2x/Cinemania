cmake_minimum_required(VERSION 3.20)
project(Cinemania LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Cinemania requires Windows and the MSVC toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Cinemania requires an x86 (Win32) build.")
endif()

add_library(Cinemania SHARED
    src/dll/bootstrap.cpp
    src/dll/bootstrap_diagnostics.cpp
    src/dll/executable_check.cpp
    src/dll/cinemania.def)
target_compile_features(Cinemania PRIVATE cxx_std_20)
target_compile_options(Cinemania PRIVATE /W4 /WX /permissive- /EHsc)
target_compile_definitions(Cinemania PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(Cinemania PRIVATE bcrypt shell32 ole32)
set_property(TARGET Cinemania PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
