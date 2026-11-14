cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(CircleScope VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

# The cluster nodes (Raspberry Pi) only need the node app, which does not
# use any audio device, so they can skip the plugin and its dependencies:
#   cmake -B build -DCIRCLESCOPE_NODE_ONLY=ON
option(CIRCLESCOPE_NODE_ONLY "Build only the cluster node app" OFF)

add_subdirectory(external/JUCE)

set(CIRCLESCOPE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

# macOS asks the user before an app may talk to the local network.
set(CIRCLESCOPE_PLIST "<plist version=\"1.0\"><dict><key>NSLocalNetworkUsageDescription</key><string>CircleScope sends audio and settings to its display nodes on the local network.</string></dict></plist>")

#===============================================================================
# Cluster node: clean full-screen scope fed over the network.
juce_add_gui_app(CircleScopeNode
    PRODUCT_NAME "CircleScopeNode"
    COMPANY_NAME "CircleScope"
    BUNDLE_ID "com.circlescope.node"
    PLIST_TO_MERGE "${CIRCLESCOPE_PLIST}")

target_sources(CircleScopeNode PRIVATE
    Source/Node/NodeMain.cpp
    Source/ScopeComponent.cpp)

target_compile_definitions(CircleScopeNode PRIVATE ${CIRCLESCOPE_DEFINITIONS})

target_link_libraries(CircleScopeNode
    PRIVATE
        juce::juce_gui_basics
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(CIRCLESCOPE_NODE_ONLY)
    return()
endif()

#===============================================================================
# Wall: one window divided into a grid of scopes, for working on one computer
# without a cluster. Fed over the network like a node.
juce_add_gui_app(CircleScopeWall
    PRODUCT_NAME "CircleScope Wall"
    COMPANY_NAME "CircleScope"
    BUNDLE_ID "com.circlescope.wall"
    PLIST_TO_MERGE "${CIRCLESCOPE_PLIST}")

target_sources(CircleScopeWall PRIVATE
    Source/Wall/WallMain.cpp
    Source/ScopeComponent.cpp)

target_compile_definitions(CircleScopeWall PRIVATE ${CIRCLESCOPE_DEFINITIONS})

target_link_libraries(CircleScopeWall
    PRIVATE
        juce::juce_gui_basics
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#===============================================================================
# Control center: live grid of every node's outputs.
juce_add_gui_app(CircleScopeControl
    PRODUCT_NAME "CircleScope Control"
    COMPANY_NAME "CircleScope"
    BUNDLE_ID "com.circlescope.control"
    PLIST_TO_MERGE "${CIRCLESCOPE_PLIST}")

target_sources(CircleScopeControl PRIVATE Source/Control/ControlMain.cpp)
target_compile_definitions(CircleScopeControl PRIVATE ${CIRCLESCOPE_DEFINITIONS})

target_link_libraries(CircleScopeControl
    PRIVATE
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#===============================================================================
# The plugin. AU only exists on macOS.
if(APPLE)
    set(CIRCLESCOPE_FORMATS AU VST3 Standalone)
else()
    set(CIRCLESCOPE_FORMATS VST3 Standalone)
endif()

juce_add_plugin(CircleScope
    COMPANY_NAME "CircleScope"
    BUNDLE_ID "com.circlescope.plugin"
    PLUGIN_MANUFACTURER_CODE Cscp
    PLUGIN_CODE Crc1
    FORMATS ${CIRCLESCOPE_FORMATS}
    PRODUCT_NAME "CircleScope"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    MICROPHONE_PERMISSION_ENABLED TRUE
    PLIST_TO_MERGE "${CIRCLESCOPE_PLIST}"
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(CircleScope PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ScopeComponent.cpp)

target_compile_definitions(CircleScope PUBLIC
    ${CIRCLESCOPE_DEFINITIONS}
    JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(CircleScope
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
