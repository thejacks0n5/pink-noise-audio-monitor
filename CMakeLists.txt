cmake_minimum_required(VERSION 3.22)

# ==============================================================================
#  Pink Noise Audio Monitor (pnam) — Version One
#  Top-level CMake build script.
#
#  Usage (macOS, Apple Silicon):
#      cmake -B build -G Xcode -DCMAKE_BUILD_TYPE=Release
#      cmake --build build --config Release
#
#  See README.md for full build/run instructions.
# ==============================================================================

# Must be set before project(): project() creates an empty cache entry for it,
# after which a non-FORCE set() is ignored and the app targets the build
# machine's own macOS version (and will not launch on anything older).
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")

project(PinkNoiseAudioMonitor VERSION 1.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple Silicon by default; override with -DCMAKE_OSX_ARCHITECTURES=x86_64 if needed.
if (APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architecture" FORCE)
endif()

# ------------------------------------------------------------------------------
# JUCE — pulled via FetchContent
# ------------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ------------------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------------------
file(GLOB_RECURSE PNAM_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
)
file(GLOB_RECURSE PNAM_HEADER_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
)

# ------------------------------------------------------------------------------
# GUI application target
# ------------------------------------------------------------------------------
juce_add_gui_app(PinkNoiseAudioMonitor
    PRODUCT_NAME            "Pink Noise Audio Monitor"
    COMPANY_NAME             "pnam"
    BUNDLE_ID                "com.pnam.pinknoiseaudiomonitor"
    ICON_BIG                 ""
    ICON_SMALL               ""
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Pink Noise Audio Monitor needs microphone access to analyse the monitored input signal."
    HARDENED_RUNTIME_ENABLED TRUE
    # Without this entitlement a hardened-runtime (Developer ID / notarized)
    # build is denied the input device silently: no prompt, every input -inf.
    HARDENED_RUNTIME_OPTIONS "com.apple.security.device.audio-input"
    NEEDS_CURL               FALSE
    NEEDS_WEB_BROWSER        FALSE
)

target_sources(PinkNoiseAudioMonitor
    PRIVATE
        ${PNAM_SOURCE_FILES}
        ${PNAM_HEADER_FILES}
)

target_include_directories(PinkNoiseAudioMonitor
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_compile_definitions(PinkNoiseAudioMonitor
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_PUSH_NOTIFICATIONS=1
        DONT_SET_USING_JUCE_NAMESPACE=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:PinkNoiseAudioMonitor,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:PinkNoiseAudioMonitor,JUCE_VERSION>"
)

target_link_libraries(PinkNoiseAudioMonitor
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Group sources nicely in generated IDE projects (Xcode/VS)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Source
    PREFIX "Source"
    FILES ${PNAM_SOURCE_FILES} ${PNAM_HEADER_FILES}
)
