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

project(PinkNoiseAudioMonitor VERSION 1.1.0 LANGUAGES C CXX)

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
    # The icon is a compiled asset catalog (Resources/Assets.car, built from
    # Resources/Assets.xcassets), not ICON_BIG: given a PNG, JUCE generates its
    # own .icns from it, the hand-fitted 16/32 px artwork is lost, and current
    # macOS draws a plain .icns shrunk onto a plate at small sizes. To rebuild
    # the catalog after changing the artwork:
    #   xcrun actool Resources/Assets.xcassets --compile Resources --platform macosx \
    #     --minimum-deployment-target 12.0 --app-icon AppIcon \
    #     --output-partial-info-plist /tmp/icon-partial.plist
    ICON_BIG                 ""
    ICON_SMALL               ""
    PLIST_TO_MERGE           "<plist version=\"1.0\"><dict><key>CFBundleIconName</key><string>AppIcon</string><key>CFBundleIconFile</key><string>AppIcon</string></dict></plist>"
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

set(PNAM_ICON_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Assets.car"
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.icns"
)
target_sources(PinkNoiseAudioMonitor PRIVATE ${PNAM_ICON_FILES})
set_source_files_properties(${PNAM_ICON_FILES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

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

# ------------------------------------------------------------------------------
# Offline tests: the real generator through a simulated signal path into the
# real artifact detector, with faults injected at known moments. Not part of
# the default build:
#     cmake --build build --target pnam_tests && ./build/pnam_tests_artefacts/<config>/pnam_tests
# Exit code = number of failed checks. Build it Release; Debug takes minutes.
# ------------------------------------------------------------------------------
juce_add_console_app(pnam_tests PRODUCT_NAME "pnam_tests")

target_sources(pnam_tests
    PRIVATE
        Tests/ArtifactDetectorTests.cpp
        Source/Core/ArtifactDetector.cpp
        Source/Core/PinkNoiseGenerator.cpp
        Source/Core/ReferenceHistory.cpp
        Source/Model/DataModel.cpp
)

target_compile_definitions(pnam_tests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(pnam_tests
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

set_target_properties(pnam_tests PROPERTIES EXCLUDE_FROM_ALL TRUE)

