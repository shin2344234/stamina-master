cmake_minimum_required(VERSION 3.20)
project(StaminaMaster LANGUAGES CXX)

# Stamina Master: an ASI plugin for Crimson Desert that reduces or removes
# the stamina cost of sprinting, climbing, swimming, gliding and riding.
#
# This build is the probe described in private/FEASIBILITY.md. It reads the
# running game's statusinfo and skill tables and writes a report; it changes
# nothing. The file it produces is StaminaMaster.asi.

if(NOT WIN32)
    message(FATAL_ERROR "Stamina Master targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# MinHook is here only for its HDE64 length decoder, which farhook uses to
# measure the bytes it steals. No MinHook hook is ever created.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# StaminaMaster.ini's own bytes, as a header, so a DMM install (which is the
# plugin alone) can have the documented ini written beside it on first run
# without a second copy of the text going out of step with the first.
# embed_ini.cmake reads and writes HEX on purpose: carrying the file through
# CMake as a string rewrites every LF as CRLF.
set(SM_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ini_default.h")
add_custom_command(
    OUTPUT "${SM_INI_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} "-DIN=${CMAKE_CURRENT_SOURCE_DIR}/StaminaMaster.ini"
                             "-DOUT=${SM_INI_HEADER}"
                             -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ini.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/StaminaMaster.ini"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_ini.cmake"
    COMMENT "Embedding StaminaMaster.ini"
    VERBATIM
)

add_library(StaminaMaster SHARED
    "${SM_INI_HEADER}"
    src/dllmain.cpp
    src/core/mod.cpp
    src/core/log.cpp
    src/core/paths.cpp
    src/game/mem.cpp
    src/game/farhook.cpp
    src/game/tables.cpp
    src/game/stamina.cpp
)

target_include_directories(StaminaMaster PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated" ${minhook_SOURCE_DIR}/src/hde)
target_link_libraries(StaminaMaster PRIVATE minhook)
target_compile_definitions(StaminaMaster PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(StaminaMaster PRIVATE /utf-8 /W3)

set_target_properties(StaminaMaster PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "StaminaMaster")

if(MSVC)
    target_compile_options(StaminaMaster PRIVATE /guard:cf)
    target_link_options(StaminaMaster PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
endif()

add_custom_command(TARGET StaminaMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:StaminaMaster>" "${CMAKE_SOURCE_DIR}/dist/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/StaminaMaster.ini" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
