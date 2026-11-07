# Run by ctest on macOS (native/mac.cmake, test auval):
#   cmake -DCOMPONENT=<SessionScope.component> -P run_auval.cmake
# Apple's auval only finds Audio Units in the Components folders, so the component is copied to
# ~/Library/Audio/Plug-Ins/Components first. macOS caches the list of components; restarting its
# "registrar" makes it look again. ctest then checks for "AU VALIDATION SUCCEEDED" in the output.

set(components "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
file(MAKE_DIRECTORY "${components}")
file(REMOVE_RECURSE "${components}/SessionScope.component")
file(COPY "${COMPONENT}" DESTINATION "${components}")
execute_process(COMMAND killall -9 AudioComponentRegistrar OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND auval -v aufx SScp Trvn OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
