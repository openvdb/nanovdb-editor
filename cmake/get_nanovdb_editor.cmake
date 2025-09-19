# Include guard to prevent multiple inclusion
if(DEFINED _GET_NANOVDB_EDITOR_CMAKE_INCLUDED)
  return()
endif()
set(_GET_NANOVDB_EDITOR_CMAKE_INCLUDED TRUE)

# Ensure Python is available
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

# Find the nanovdb_editor Python package directory
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONPATH="${Python3_SITELIB}" "${Python3_EXECUTABLE}" -c "import os; import nanovdb_editor; print(os.path.dirname(nanovdb_editor.__file__))"
  OUTPUT_VARIABLE NANOVDB_EDITOR_PACKAGE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE NANOVDB_EDITOR_IMPORT_RESULT)

if(NOT NANOVDB_EDITOR_IMPORT_RESULT EQUAL 0 OR NOT EXISTS "${NANOVDB_EDITOR_PACKAGE_DIR}")
  message(FATAL_ERROR "Failed to import nanovdb_editor. Please ensure the nanovdb_editor wheel is installed in the active Python environment.")
endif()

# Derive include and lib directories from the package
set(NANOVDB_EDITOR_INCLUDE_DIR "${NANOVDB_EDITOR_PACKAGE_DIR}/include")
set(NANOVDB_EDITOR_LIB_DIR     "${NANOVDB_EDITOR_PACKAGE_DIR}")

if(NOT EXISTS "${NANOVDB_EDITOR_INCLUDE_DIR}")
  message(FATAL_ERROR "nanovdb_editor include directory not found: ${NANOVDB_EDITOR_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${NANOVDB_EDITOR_LIB_DIR}")
  message(FATAL_ERROR "nanovdb_editor library directory not found: ${NANOVDB_EDITOR_LIB_DIR}")
endif()

# Report discovery
message(STATUS "NANOVDB_EDITOR_PACKAGE_DIR: ${NANOVDB_EDITOR_PACKAGE_DIR}")
message(STATUS "NANOVDB_EDITOR_INCLUDE_DIR: ${NANOVDB_EDITOR_INCLUDE_DIR}")
message(STATUS "NANOVDB_EDITOR_LIB_DIR: ${NANOVDB_EDITOR_LIB_DIR}")

# Helper to pick a library file considering platform prefixes/suffixes
set(_SHARED_SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}")
set(_SHARED_PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}")
set(_STATIC_SUFFIX "${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_STATIC_PREFIX "${CMAKE_STATIC_LIBRARY_PREFIX}")

# Libraries expected from the wheel
set(_NANOVDB_EDITOR_COMPONENTS
  pnanovdbcompiler
  pnanovdbcompute
  pnanovdbfileformat
  pnanovdbeditor)

set(NANOVDB_EDITOR_LIBRARIES)
set(NANOVDB_EDITOR_INCLUDE_DIRS "${NANOVDB_EDITOR_INCLUDE_DIR}")

foreach(_comp ${_NANOVDB_EDITOR_COMPONENTS})
  set(_shared_candidate "${NANOVDB_EDITOR_LIB_DIR}/${_SHARED_PREFIX}${_comp}${_SHARED_SUFFIX}")
  set(_static_candidate "${NANOVDB_EDITOR_LIB_DIR}/${_STATIC_PREFIX}${_comp}${_STATIC_SUFFIX}")
  set(_chosen_lib "")

  if(EXISTS "${_shared_candidate}")
    set(_chosen_lib "${_shared_candidate}")
  elseif(EXISTS "${_static_candidate}")
    set(_chosen_lib "${_static_candidate}")
  endif()

  if(NOT _chosen_lib)
    message(FATAL_ERROR "nanovdb_editor library not found for component '${_comp}' in ${NANOVDB_EDITOR_LIB_DIR}")
  endif()

  # Expose per-component variable
  string(TOUPPER "${_comp}" _COMP_UPPER)
  set("NANOVDB_EDITOR_LIBRARY_${_COMP_UPPER}" "${_chosen_lib}")

  # Create imported target for the component
  set(_target_name "nanovdb_editor::${_comp}")
  if(NOT TARGET ${_target_name})
    add_library(${_target_name} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(${_target_name} PROPERTIES
      IMPORTED_LOCATION "${_chosen_lib}")
    target_include_directories(${_target_name} INTERFACE "${NANOVDB_EDITOR_INCLUDE_DIR}")
  endif()

  list(APPEND NANOVDB_EDITOR_LIBRARIES "${_chosen_lib}")
endforeach()

# Aggregate, convenient imported interface target for consumers
if(NOT TARGET nanovdb_editor::nanovdb_editor)
  add_library(nanovdb_editor::nanovdb_editor INTERFACE IMPORTED)
  set_target_properties(nanovdb_editor::nanovdb_editor PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NANOVDB_EDITOR_INCLUDE_DIR}")
  # Link all component targets
  set_property(TARGET nanovdb_editor::nanovdb_editor APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES
      nanovdb_editor::pnanovdbcompiler
      nanovdb_editor::pnanovdbcompute
      nanovdb_editor::pnanovdbfileformat
      nanovdb_editor::pnanovdbeditor)
endif()

# Final status output
message(STATUS "NANOVDB_EDITOR_LIBRARIES: ${NANOVDB_EDITOR_LIBRARIES}")
