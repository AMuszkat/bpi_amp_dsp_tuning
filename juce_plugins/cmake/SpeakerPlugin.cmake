# Shared CMake setup for every plugin in juce_plugins/.
#
# Each plugin's CMakeLists.txt is just:
#
#     cmake_minimum_required(VERSION 3.22)
#     project(MY_PLUGIN VERSION 0.0.1)
#     find_package(JUCE CONFIG REQUIRED)
#     include(${CMAKE_CURRENT_LIST_DIR}/../cmake/SpeakerPlugin.cmake)
#
#     speaker_add_plugin(myPlugin
#         CODE    Mypl                  # unique 4-char plugin code
#         INPUTS  1                     # main-bus channel counts (see below)
#         OUTPUTS 1
#         SOURCES PluginProcessor.cpp PluginEditor.cpp
#         MODULES juce_dsp)             # JUCE modules beyond the common set, optional
#
# and anything genuinely plugin-specific (an extra library, a define) goes after the call,
# against the same target.
#
# The target name doubles as the product name, the LV2 binary name and the last segment of
# the LV2 URI, and tools/build-podman.sh relies on that: it builds `<name>_LV2` and expects
# `<name>.lv2`.

include_guard(GLOBAL)

# Base of every plugin's LV2 URI. The URI is the plugin's global identity: hosts key their
# plugin database on it, so two plugins sharing one collide and only one survives a scan,
# and changing it orphans every saved session that references the old one. Hence one base
# for the whole repo, with the plugin name appended -- unique by construction. Change it
# deliberately, once (e.g. to the public repository URL), not per machine.
# tools/build-podman.sh reads this default back to write its debug sessions, so keep it on
# one line in this exact form.
# (A plain variable, not a cache entry: a cached value would silently survive an edit here in
# every existing build tree.)
set(SPEAKER_LV2_URI_BASE "https://github.com/AMuszkat/bpi_amp_dsp_tuning")

# JUCE modules every plugin links. A plugin adds more through MODULES.
set(SPEAKER_COMMON_MODULES
    juce_audio_utils
    juce_audio_processors
    juce_gui_basics
    juce_gui_extra)

function(speaker_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "CODE;INPUTS;OUTPUTS" "SOURCES;MODULES")

    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "speaker_add_plugin(${target}): unknown arguments ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT ARG_CODE MATCHES "^[A-Za-z0-9][a-z0-9][a-z0-9][a-z0-9]$")
        # JUCE wants exactly four characters; GarageBand additionally wants only the first
        # upper-case. VST3/AU hosts key on the code, so it MUST differ between plugins.
        message(FATAL_ERROR "speaker_add_plugin(${target}): CODE must be 4 characters, "
                            "only the first one upper-case (got '${ARG_CODE}')")
    endif()
    # INPUTS / OUTPUTS are the main-bus channel counts. They must match the processor's
    # BusesProperties; CMake does not use them itself -- tools/build-podman.sh reads them
    # from this call to wire the plugin into the debug plugin-host session.
    foreach(n IN ITEMS INPUTS OUTPUTS)
        if(NOT ARG_${n} MATCHES "^[0-9]+$")
            message(FATAL_ERROR "speaker_add_plugin(${target}): ${n} must be a channel count")
        endif()
    endforeach()
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "speaker_add_plugin(${target}): SOURCES is required")
    endif()

    juce_add_plugin(${target}
        COPY_PLUGIN_AFTER_BUILD TRUE
        PLUGIN_MANUFACTURER_CODE Juce          # may be shared by all plugins
        PLUGIN_CODE ${ARG_CODE}
        FORMATS LV2 VST3
        PRODUCT_NAME "${target}"
        LV2URI "${SPEAKER_LV2_URI_BASE}/${target}"
        LV2_SHARED_LIBRARY_NAME ${target})

    juce_generate_juce_header(${target})

    target_sources(${target} PRIVATE ${ARG_SOURCES})

    target_compile_definitions(${target}
        PUBLIC
            JUCE_WEB_BROWSER=0                 # re-enabling needs NEEDS_WEB_BROWSER TRUE above
            JUCE_USE_CURL=0                    # re-enabling needs NEEDS_CURL TRUE above
            JUCE_VST3_CAN_REPLACE_VST2=0)

    set(modules ${SPEAKER_COMMON_MODULES} ${ARG_MODULES})
    list(REMOVE_DUPLICATES modules)
    list(TRANSFORM modules PREPEND "juce::")
    target_link_libraries(${target}
        PRIVATE
            ${modules}
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Link-time optimisation, parallelised. This replaces juce::juce_recommended_lto_flags:
    # for GCC/Clang that passes a bare `-flto`, which runs the LTO link phase (LTRANS)
    # single-threaded -- the slowest part of the build. `-flto=auto` produces the same
    # binary with one LTRANS job per core. MSVC keeps the usual -GL/-LTCG.
    if(MSVC)
        target_compile_options(${target} PUBLIC $<$<CONFIG:Release>:-GL>)
        target_link_options(${target} PUBLIC $<$<CONFIG:Release>:-LTCG>)
    else()
        target_compile_options(${target} PUBLIC $<$<CONFIG:Release>:-flto=auto>)
        target_link_options(${target} PUBLIC $<$<CONFIG:Release>:-flto=auto>)
    endif()
endfunction()
