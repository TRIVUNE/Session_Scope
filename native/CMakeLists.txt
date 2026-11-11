# =================================================================================================
# native/CMakeLists.txt - the plug-ins: the sensor in every format, and the REAPER extension
# =================================================================================================
#
# Windows (into build/native/):
#   SessionScope.vst3.dll     the VST3 sensor (installed as SessionScope.vst3/Contents/x86_64-win/...)
#   SessionScope.vst3/...     the same, as a ready VST3 bundle folder (what the validator checks)
#   SessionScope.clap         the CLAP sensor (a CLAP plug-in on Windows is a DLL named .clap)
#   SessionScopeProbe.vst3... the VST3 Probe: holds one VST3 effect and times it (vst3_probe/)
#   SessionScopeProbe.clap    the CLAP Probe: a timed twin of every installed CLAP plug-in (clap_probe/)
#   reaper_sessionscope.dll   the optional REAPER extension (REAPER's own CPU per block)
# macOS (into build/native/mac/), each a universal bundle (Apple silicon and Intel), signed ad hoc:
#   SessionScope.vst3, SessionScope.clap, SessionScope.component (Audio Unit)
#
# Checks, run with:  ctest --preset windows-plugins   (or mac-plugins)
#   vst3_validator    Steinberg's official validator (built here from the VST3 SDK)
#   clap_validator    the official clap-validator (downloaded, pinned)
#   clap_host         a small host that loads the CLAP sensor and reads its records back
#   vst3_probe_*      the validator on the VST3 Probe, and two small hosts that run audio through it
#   clap_probe_*      clap-validator on the CLAP Probe (twinning our sensor), and a small host
# They use the real shared memory, so close any DAW that has the sensor loaded first.
#
# Third-party code, fetched once into .toolchain/ (cmake/fetch.cmake), pinned:
#   VST3 SDK 3.8.1 (MIT)            github.com/steinbergmedia/vst3sdk
#   CLAP 1.2.9 (MIT)                github.com/free-audio/clap, headers only
#   clap-validator 0.4.1 (MIT)      github.com/free-audio/clap-validator
#   AudioUnitSDK 1.4.0 (Apache 2.0) github.com/apple/AudioUnitSDK, macOS only
#   REAPER SDK header                kept in the repository (hosts/reaper/third_party/reaper-sdk)

fetch_git(vst3sdk https://github.com/steinbergmedia/vst3sdk.git v3.8.1_build_84 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96 RECURSIVE)
fetch_git(clap https://github.com/free-audio/clap 1.2.9 cd94482ba5941ae410809b6fbaed3bc851044270)
set(VST3_SDK "${TOOLCHAIN_DIR}/vst3sdk")
set(CLAP_INCLUDE "${TOOLCHAIN_DIR}/clap/include")
set(NATIVE_OUT "${CMAKE_SOURCE_DIR}/build/native")

# Warnings on for our plug-in code (not turned into errors: the SDK headers we include are noisy).
set(PLUGIN_WARNINGS -Wall -Wextra)

# The VST3 SDK files a plug-in needs (the same list for the sensor and the probe, later).
set(vst3_plugin_files
    public.sdk/source/main/pluginfactory.cpp public.sdk/source/vst/vstsinglecomponenteffect.cpp
    public.sdk/source/vst/vstcomponentbase.cpp public.sdk/source/vst/vstbus.cpp public.sdk/source/vst/vstinitiids.cpp
    public.sdk/source/vst/vstparameters.cpp public.sdk/source/common/pluginview.cpp public.sdk/source/common/commoniids.cpp
    base/source/fobject.cpp base/source/fstring.cpp base/source/baseiids.cpp base/source/fdebug.cpp
    base/source/updatehandler.cpp base/thread/source/flock.cpp
    pluginterfaces/base/funknown.cpp pluginterfaces/base/coreiids.cpp pluginterfaces/base/conststringtable.cpp
    pluginterfaces/base/ustring.cpp)
if(APPLE)
  list(APPEND vst3_plugin_files public.sdk/source/main/macmain.cpp)
else()
  list(APPEND vst3_plugin_files public.sdk/source/main/dllmain.cpp)
endif()
list(TRANSFORM vst3_plugin_files PREPEND "${VST3_SDK}/")
# Steinberg's files print a few warnings of their own; they aren't ours to fix, so they're silenced
# for those files only (our sensor code keeps every warning on).
set_source_files_properties(${vst3_plugin_files} PROPERTIES COMPILE_OPTIONS -w)

# ---- the sensor's shared-memory test (plain C++, no plug-in format)
add_executable(test_sensor_ring sensor_core/test_sensor_ring.cpp)
target_compile_options(test_sensor_ring PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_sensor_ring PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
add_test(NAME test_sensor_ring COMMAND test_sensor_ring)

if(APPLE)
  include("${CMAKE_CURRENT_LIST_DIR}/mac.cmake")   # bundles, Audio Unit, auval
  return()
endif()

# =================================================================================================
# Windows
# =================================================================================================

# ---- the VST3 sensor
add_library(vst3_sensor SHARED vst3_sensor/sessionscope_sensor.cpp ${vst3_plugin_files})
target_include_directories(vst3_sensor SYSTEM PRIVATE "${VST3_SDK}")
target_compile_definitions(vst3_sensor PRIVATE RELEASE=1 NDEBUG _CRT_SECURE_NO_WARNINGS)
target_compile_options(vst3_sensor PRIVATE ${PLUGIN_WARNINGS})
target_link_libraries(vst3_sensor PRIVATE ole32)
set_target_properties(vst3_sensor PROPERTIES PREFIX "" OUTPUT_NAME "SessionScope.vst3" SUFFIX ".dll"
                                             RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
# A VST3 is a folder ("bundle") with the DLL inside, named .vst3. Hosts and the validator want that.
set(vst3_bundle "${NATIVE_OUT}/SessionScope.vst3/Contents/x86_64-win")
add_custom_command(TARGET vst3_sensor POST_BUILD
                   COMMAND "${CMAKE_COMMAND}" -E make_directory "${vst3_bundle}"
                   COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:vst3_sensor>" "${vst3_bundle}/SessionScope.vst3"
                   COMMENT "Making the SessionScope.vst3 bundle folder")

# ---- the CLAP sensor
add_library(clap_sensor SHARED clap_sensor/sessionscope_clap.cpp)
target_include_directories(clap_sensor SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_definitions(clap_sensor PRIVATE NDEBUG)
target_compile_options(clap_sensor PRIVATE ${PLUGIN_WARNINGS})
set_target_properties(clap_sensor PROPERTIES PREFIX "" OUTPUT_NAME "SessionScope" SUFFIX ".clap"
                                             RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")

# ---- the REAPER extension (decision 0003): REAPER loads reaper_*.dll from its UserPlugins folder
add_library(reaper_extension SHARED ../hosts/reaper/extension/sessionscope_ext.cpp)
target_include_directories(reaper_extension SYSTEM PRIVATE ../hosts/reaper/third_party/reaper-sdk)
target_compile_options(reaper_extension PRIVATE ${PLUGIN_WARNINGS})
set_target_properties(reaper_extension PROPERTIES PREFIX "" OUTPUT_NAME "reaper_sessionscope"
                                                  RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")

# ---- checks: Steinberg's VST3 validator, built from the SDK's own sources
set(validator_dir "${VST3_SDK}/public.sdk/samples/vst-hosting/validator/source")
set(vst3_validator_files
    "${validator_dir}/main.cpp" "${validator_dir}/usediids.cpp" "${validator_dir}/validator.cpp"
    vst3_validator/winmain_wrapper.cpp)   # Zig's startup code wants WinMain; the validator has wmain
set(sdk_files
    public.sdk/source/vst/hosting/connectionproxy.cpp public.sdk/source/vst/hosting/eventlist.cpp
    public.sdk/source/vst/hosting/hostclasses.cpp public.sdk/source/vst/hosting/module.cpp
    public.sdk/source/vst/hosting/module_win32.cpp public.sdk/source/vst/hosting/parameterchanges.cpp
    public.sdk/source/vst/hosting/pluginterfacesupport.cpp public.sdk/source/vst/hosting/processdata.cpp
    public.sdk/source/vst/hosting/plugprovider.cpp
    public.sdk/source/vst/moduleinfo/moduleinfoparser.cpp public.sdk/source/common/commoniids.cpp
    public.sdk/source/common/commonstringconvert.cpp public.sdk/source/common/memorystream.cpp
    public.sdk/source/common/openurl.cpp public.sdk/source/common/readfile.cpp
    public.sdk/source/common/systemclipboard_win32.cpp public.sdk/source/common/threadchecker_win32.cpp
    public.sdk/source/vst/vstpresetfile.cpp public.sdk/source/vst/vstinitiids.cpp
    base/source/baseiids.cpp base/source/fbuffer.cpp base/source/fdebug.cpp base/source/fdynlib.cpp
    base/source/fobject.cpp base/source/fstreamer.cpp base/source/fstring.cpp base/source/timer.cpp
    base/source/updatehandler.cpp base/thread/source/fcondition.cpp base/thread/source/flock.cpp
    pluginterfaces/base/conststringtable.cpp pluginterfaces/base/coreiids.cpp pluginterfaces/base/funknown.cpp
    pluginterfaces/base/ustring.cpp
    public.sdk/source/vst/testsuite/testbase.cpp public.sdk/source/vst/testsuite/vsttestsuite.cpp
    public.sdk/source/vst/utility/testing.cpp public.sdk/source/vst/utility/stringconvert.cpp)
# The test suite: every test file in these folders.
foreach(group bus general noteexpression processing state unit)
  file(GLOB group_files "${VST3_SDK}/public.sdk/source/vst/testsuite/${group}/*.cpp")
  list(APPEND vst3_validator_files ${group_files})
endforeach()
list(TRANSFORM sdk_files PREPEND "${VST3_SDK}/")
add_library(vst3_host_sdk OBJECT ${sdk_files})
target_include_directories(vst3_host_sdk SYSTEM PUBLIC "${VST3_SDK}" "${VST3_SDK}/public.sdk"
                           "${VST3_SDK}/public.sdk/source/vst/hosting" "${VST3_SDK}/public.sdk/source/vst/testsuite")
target_compile_definitions(vst3_host_sdk PUBLIC SMTG_OS_WINDOWS=1 COM_COMPATIBLE=1 _CRT_SECURE_NO_WARNINGS RELEASE=1 NDEBUG)
target_compile_options(vst3_host_sdk PRIVATE -w)
target_link_libraries(vst3_host_sdk PUBLIC ole32 uuid shell32 shlwapi advapi32)

add_executable(vst3_validator ${vst3_validator_files} "${VST3_SDK}/public.sdk/source/main/moduleinit.cpp")
target_link_libraries(vst3_validator PRIVATE vst3_host_sdk -Wl,--subsystem=console)
target_compile_options(vst3_validator PRIVATE -w)   # Steinberg's code: not ours to fix
set_target_properties(vst3_validator PROPERTIES OUTPUT_NAME validator RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
add_test(NAME vst3_validator COMMAND vst3_validator "${NATIVE_OUT}/SessionScope.vst3")
set_tests_properties(vst3_validator PROPERTIES LABELS plugins PASS_REGULAR_EXPRESSION "Result: [0-9]+ tests passed, 0 tests failed")

# ---- the VST3 Probe: a plug-in that holds another VST3 effect and times it (Windows picker window)
# It is a plug-in (the SDK's plug-in files) and a host at once (the SDK's hosting files, built
# once above as vst3_host_sdk for the validator too). A file in both lists is taken from the
# hosting library only, or the linker would see it twice.
set(probe_plugin_files ${vst3_plugin_files})
list(REMOVE_ITEM probe_plugin_files ${sdk_files})
add_library(vst3_probe SHARED vst3_probe/sessionscope_probe.cpp vst3_probe/hosted_vst3.cpp
                             ${probe_plugin_files} "${validator_dir}/usediids.cpp")
target_link_libraries(vst3_probe PRIVATE vst3_host_sdk)
target_compile_options(vst3_probe PRIVATE ${PLUGIN_WARNINGS})
set_target_properties(vst3_probe PROPERTIES PREFIX "" OUTPUT_NAME "SessionScopeProbe.vst3" SUFFIX ".dll"
                                            RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
set(probe_bundle "${NATIVE_OUT}/SessionScopeProbe.vst3")
add_custom_command(TARGET vst3_probe POST_BUILD
                   COMMAND "${CMAKE_COMMAND}" -E make_directory "${probe_bundle}/Contents/x86_64-win"
                   COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:vst3_probe>"
                           "${probe_bundle}/Contents/x86_64-win/SessionScopeProbe.vst3")
add_test(NAME vst3_probe_validator COMMAND vst3_validator "${probe_bundle}")
set_tests_properties(vst3_probe_validator PROPERTIES LABELS plugins
                     PASS_REGULAR_EXPRESSION "Result: [0-9]+ tests passed, 0 tests failed")

# ---- checks: load and time the real Sensor through the Probe hosting core
add_executable(test_hosted_vst3 vst3_probe/hosted_vst3.cpp vst3_probe/test_hosted_vst3.cpp
                              "${validator_dir}/usediids.cpp")
target_link_libraries(test_hosted_vst3 PRIVATE vst3_host_sdk -Wl,--subsystem=console)
target_compile_options(test_hosted_vst3 PRIVATE ${PLUGIN_WARNINGS})
add_dependencies(test_hosted_vst3 vst3_sensor)
add_test(NAME vst3_probe_host COMMAND test_hosted_vst3 "${NATIVE_OUT}/SessionScope.vst3")
set_tests_properties(vst3_probe_host PROPERTIES LABELS plugins)

add_executable(test_probe_bundle vst3_probe/hosted_vst3.cpp vst3_probe/test_probe_bundle.cpp
                                 "${validator_dir}/usediids.cpp")
target_link_libraries(test_probe_bundle PRIVATE vst3_host_sdk -Wl,--subsystem=console)
target_compile_options(test_probe_bundle PRIVATE ${PLUGIN_WARNINGS})
add_dependencies(test_probe_bundle vst3_probe vst3_sensor)
add_test(NAME vst3_probe_bundle COMMAND test_probe_bundle "${probe_bundle}" "${NATIVE_OUT}/SessionScope.vst3")
set_tests_properties(vst3_probe_bundle PROPERTIES LABELS plugins)

# ---- checks: clap-validator (a downloaded program) and our small CLAP host
fetch_from_zip(clap-validator
               https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-windows.zip
               d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10 clap-validator.exe)
# Its JSON report has one "code" per test: "success", "skipped" (a feature the sensor doesn't have),
# or a failure. The check passes if something succeeded and nothing failed or crashed.
#
# The denormals test runs on its own, and there a warning is allowed. It times the plug-in on
# ordinary numbers and on "denormals" (numbers so close to zero that some processors compute with
# them slowly) and warns when the second is much slower. Our plug-ins only copy the audio, never
# calculate with it, and one block takes well under a microsecond, so that comparison measures the
# clock's own noise: on some computers it warns on every run. A real failure still fails.
set(CLAP_VALIDATOR "${TOOLCHAIN_DIR}/clap-validator/clap-validator.exe")
function(add_clap_validator_tests name plugin_file)
  add_test(NAME ${name} COMMAND "${CLAP_VALIDATOR}" validate --json -x process-audio-denormals "${plugin_file}")
  set_tests_properties(${name} PROPERTIES LABELS plugins
                       PASS_REGULAR_EXPRESSION "\"code\": *\"success\""
                       FAIL_REGULAR_EXPRESSION "\"code\": *\"(failed|crashed|warning)\"")
  add_test(NAME ${name}_denormals COMMAND "${CLAP_VALIDATOR}" validate --json -t process-audio-denormals "${plugin_file}")
  set_tests_properties(${name}_denormals PROPERTIES LABELS plugins
                       PASS_REGULAR_EXPRESSION "\"code\": *\"(success|warning)\""
                       FAIL_REGULAR_EXPRESSION "\"code\": *\"(failed|crashed)\"")
endfunction()
add_clap_validator_tests(clap_validator "$<TARGET_FILE:clap_sensor>")

add_executable(test_clap_sensor clap_sensor/test_clap_sensor.cpp)
target_include_directories(test_clap_sensor SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_options(test_clap_sensor PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_clap_sensor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
add_test(NAME clap_host COMMAND test_clap_sensor "$<TARGET_FILE:clap_sensor>")
set_tests_properties(clap_host PROPERTIES LABELS plugins)

# ---- the CLAP Probe: shows the DAW a timed "twin" of every installed CLAP plug-in
add_library(clap_probe SHARED clap_probe/sessionscope_clap_probe.cpp)
target_include_directories(clap_probe SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_definitions(clap_probe PRIVATE NDEBUG)
target_compile_options(clap_probe PRIVATE -Wall -Wextra -Werror)
set_target_properties(clap_probe PROPERTIES PREFIX "" OUTPUT_NAME "SessionScopeProbe" SUFFIX ".clap"
                                            RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")

# Its checks. The Probe makes twins of the plug-ins it finds in the standard CLAP folders; for the
# checks SESSIONSCOPE_PROBE_CLAP_PATH points it at build/native only, where it finds exactly one
# plug-in to twin: our own CLAP sensor. So clap-validator validates "the sensor, probed", and the
# checks don't depend on (or load) whatever else is installed on the computer.
add_clap_validator_tests(clap_probe_validator "$<TARGET_FILE:clap_probe>")
add_executable(test_clap_probe clap_probe/test_clap_probe.cpp)
target_include_directories(test_clap_probe SYSTEM PRIVATE "${CLAP_INCLUDE}")
target_compile_options(test_clap_probe PRIVATE -Wall -Wextra -Werror)
set_target_properties(test_clap_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${NATIVE_OUT}")
add_dependencies(test_clap_probe clap_probe clap_sensor)
add_test(NAME clap_probe_host COMMAND test_clap_probe "$<TARGET_FILE:clap_probe>")
set_tests_properties(clap_probe_host PROPERTIES LABELS plugins)
set_tests_properties(clap_probe_validator clap_probe_validator_denormals clap_probe_host PROPERTIES
                     ENVIRONMENT "SESSIONSCOPE_PROBE_CLAP_PATH=${NATIVE_OUT}")
