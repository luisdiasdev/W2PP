# Copies a freshly built executable - and its PDB, when the configuration
# emits one - into the legacy release tree used by the VS Code launch
# configurations in .vscode/launch.json.
#
# Invoked from a POST_BUILD step with:
#   -DEXE_FILE=<path>    (required)
#   -DPDB_FILE=<path>    (may be empty for configurations without symbols)
#   -DDEST_DIR=<dir>     (required)

if(NOT EXE_FILE OR NOT EXISTS "${EXE_FILE}")
  message(FATAL_ERROR "copy_to_release: executable '${EXE_FILE}' not found")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

function(copy_if_different source)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${DEST_DIR}/"
    RESULT_VARIABLE result
    ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR
      "copy_to_release: failed to copy '${source}' to '${DEST_DIR}': ${error}")
  endif()
endfunction()

copy_if_different("${EXE_FILE}")
if(PDB_FILE AND EXISTS "${PDB_FILE}")
  copy_if_different("${PDB_FILE}")
endif()
