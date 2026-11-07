# Run by ctest on macOS (native/mac.cmake, test vst3_validator):
#   cmake -DBUILD_DIR=<build/cmake> -DVALIDATOR_DIR=<.../bin> -DPLUGIN=<SessionScope.vst3> -P run_vst3_validator.cmake
# Builds Steinberg's validator the first time (from the VST3 SDK, with the SDK's own CMake), then
# runs it on the bundle. Fails if the validator reports a failed test.

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target vst3_validator_build RESULT_VARIABLE failed)
if(failed)
  message(FATAL_ERROR "Could not build Steinberg's validator.")
endif()
file(GLOB_RECURSE validator "${VALIDATOR_DIR}/validator")
if(NOT validator)
  message(FATAL_ERROR "No validator found under ${VALIDATOR_DIR}.")
endif()
list(GET validator 0 validator)
execute_process(COMMAND "${validator}" "${PLUGIN}" RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(result)
  message(FATAL_ERROR "The VST3 validator reported a failure.")
endif()
