# =================================================================================================
# native/mac.cmake - the sensor as macOS plug-in bundles (included by native/CMakeLists.txt)
# =================================================================================================
#
# Builds, into build/native/mac/:
#   SessionScope.vst3/Contents/MacOS/SessionScope        for VST3 hosts
#   SessionScope.clap/Contents/MacOS/SessionScope        for CLAP hosts
#   SessionScope.component/Contents/MacOS/SessionScope   for Audio Unit hosts (Logic, GarageBand...)
# Each is a universal binary (Apple silicon and Intel), with its Info.plist, signed "ad hoc" (no
# developer account needed; Apple silicon refuses to load unsigned code).
#
# NOT YET RUN: this replaced tools/build_native_mac.py and tools/check_mac_plugins.py on 7 Nov 2026,
# written from them line by line, but nobody has built it on a Mac yet.

fetch_git(AudioUnitSDK https://github.com/apple/AudioUnitSDK AudioUnitSDK-1.4.0 bd98b31feff57a15989fcfab4cd86dc63382b1ac)
set(AU_SDK "${TOOLCHAIN_DIR}/AudioUnitSDK")
set(MAC_OUT "${NATIVE_OUT}/mac")
set(SENSOR_VERSION 0.3.0)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")   # universal: both kinds of Mac

# make_bundle(<target> <extension> <bundle id> <plist template>): turns a target into a bundle
# folder named SessionScope.<extension>, and signs it after every build.
function(make_bundle target extension identifier plist)
  set_target_properties(${target} PROPERTIES
    BUNDLE TRUE BUNDLE_EXTENSION ${extension} OUTPUT_NAME SessionScope
    LIBRARY_OUTPUT_DIRECTORY "${MAC_OUT}"
    MACOSX_BUNDLE_GUI_IDENTIFIER ${identifier}
    MACOSX_BUNDLE_BUNDLE_VERSION ${SENSOR_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${SENSOR_VERSION}
    MACOSX_BUNDLE_INFO_PLIST "${plist}"
    CXX_VISIBILITY_PRESET hidden)
  add_custom_command(TARGET ${target} POST_BUILD
                     COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
                     COMMENT "Signing SessionScope.${extension} (ad hoc)")
endfunction()

# ---- VST3
add_library(vst3_sensor MODULE vst3_sensor/sessionscope_sensor.cpp ${vst3_plugin_files})
target_include_directories(vst3_sensor SYSTEM PRIVATE "${VST3_SDK}")
target_compile_definitions(vst3_sensor PRIVATE RELEASE=1 NDEBUG)
target_compile_options(vst3_sensor PRIVATE ${PLUGIN_WARNINGS})
target_link_options(vst3_sensor PRIVATE "-Wl,-exported_symbols_list,${VST3_SDK}/public.sdk/source/main/macexport.exp")
target_link_libraries(vst3_sensor PRIVATE "-framework CoreFoundation" "-framework Foundation")
make_bundle(vst3_sensor vst3 com.trivune.sessionscope.vst3 "${CMAKE_CURRENT_LIST_DIR}/mac/Info.plist.in")

# ---- CLAP (only clap_entry is visible from outside: the rest is hidden)
add_library(clap_sensor MODULE clap_sensor/sessionscope_clap.cpp)
target_include_directories(clap_sensor SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_definitions(clap_sensor PRIVATE NDEBUG)
target_compile_options(clap_sensor PRIVATE ${PLUGIN_WARNINGS})
make_bundle(clap_sensor clap com.trivune.sessionscope.clap "${CMAKE_CURRENT_LIST_DIR}/mac/Info.plist.in")

# ---- Audio Unit: needs C++23 (the SDK uses it); its plist also says which kind of AU it is
# (type aufx = effect, subtype SScp, manufacturer Trvn; version 0x000300 = 768 for 0.3.0).
file(GLOB au_sdk_files "${AU_SDK}/src/AudioUnitSDK/*.cpp")
add_library(au_sensor MODULE au_sensor/sessionscope_au.cpp ${au_sdk_files})
target_include_directories(au_sensor SYSTEM PRIVATE "${AU_SDK}/include")
target_compile_definitions(au_sensor PRIVATE NDEBUG)
set_target_properties(au_sensor PROPERTIES CXX_STANDARD 23)
# The SDK's render methods are marked real-time safe ("nonblocking"); clang can't see that our
# sensor core is too, so that one check is off for this plug-in.
target_compile_options(au_sensor PRIVATE ${PLUGIN_WARNINGS} -Wno-unknown-warning-option -Wno-function-effects)
target_link_libraries(au_sensor PRIVATE "-framework AudioToolbox" "-framework CoreFoundation" "-framework CoreAudio")
make_bundle(au_sensor component com.trivune.sessionscope.component "${CMAKE_CURRENT_LIST_DIR}/mac/Info-AU.plist.in")

# ---- checks
# VST3: Steinberg's validator, built with the SDK's own CMake (only the validator: no GUI, no examples).
include(ExternalProject)
ExternalProject_Add(vst3_validator_build
  SOURCE_DIR "${VST3_SDK}" BINARY_DIR "${CMAKE_BINARY_DIR}/vst3sdk-mac"
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF
  BUILD_COMMAND "${CMAKE_COMMAND}" --build . --target validator --config Release --parallel
  INSTALL_COMMAND "" EXCLUDE_FROM_ALL TRUE)
add_test(NAME vst3_validator COMMAND "${CMAKE_COMMAND}" -DBUILD_DIR=${CMAKE_BINARY_DIR} -DVALIDATOR_DIR=${CMAKE_BINARY_DIR}/vst3sdk-mac/bin
         -DPLUGIN=$<TARGET_BUNDLE_DIR:vst3_sensor> -P "${CMAKE_CURRENT_LIST_DIR}/mac/run_vst3_validator.cmake")
set_tests_properties(vst3_validator PROPERTIES LABELS plugins)

# CLAP: clap-validator (universal macOS build: a zip holding a .tar.gz holding the program).
fetch_file(clap-validator-macos.zip
           https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-macos-universal.zip
           bbec8cd7d18274e549d5d8c12ece3cec54be966129388dd2e742b9957f2ba9f1)
set(clap_validator_dir "${TOOLCHAIN_DIR}/clap-validator-macos")
if(NOT EXISTS "${clap_validator_dir}/clap-validator")
  file(ARCHIVE_EXTRACT INPUT "${TOOLCHAIN_DIR}/clap-validator-macos.zip" DESTINATION "${clap_validator_dir}/zip")
  file(GLOB_RECURSE tarball "${clap_validator_dir}/zip/*.tar.gz")
  file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${clap_validator_dir}/tar")
  file(GLOB_RECURSE program "${clap_validator_dir}/tar/*clap-validator")
  file(COPY ${program} DESTINATION "${clap_validator_dir}")
  file(REMOVE_RECURSE "${clap_validator_dir}/zip" "${clap_validator_dir}/tar")
endif()
add_test(NAME clap_validator COMMAND "${clap_validator_dir}/clap-validator" validate --json "$<TARGET_BUNDLE_DIR:clap_sensor>")
set_tests_properties(clap_validator PROPERTIES LABELS plugins
                     PASS_REGULAR_EXPRESSION "\"code\": *\"success\""
                     FAIL_REGULAR_EXPRESSION "\"code\": *\"(failed|crashed|warning)\"")
add_executable(test_clap_sensor clap_sensor/test_clap_sensor.cpp)
target_include_directories(test_clap_sensor SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_options(test_clap_sensor PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_clap_sensor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
add_test(NAME clap_host COMMAND test_clap_sensor "$<TARGET_BUNDLE_DIR:clap_sensor>")
set_tests_properties(clap_host PROPERTIES LABELS plugins)

# Audio Unit: Apple's auval, after copying the component where macOS looks for it.
add_test(NAME auval COMMAND "${CMAKE_COMMAND}" -DCOMPONENT=$<TARGET_BUNDLE_DIR:au_sensor>
         -P "${CMAKE_CURRENT_LIST_DIR}/mac/run_auval.cmake")
set_tests_properties(auval PROPERTIES LABELS plugins PASS_REGULAR_EXPRESSION "AU VALIDATION SUCCEEDED")
