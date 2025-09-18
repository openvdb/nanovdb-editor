# CMake script to patch cnpy CMakeLists.txt to disable examples
file(READ "CMakeLists.txt" CNPY_CMAKE_CONTENT)

# Replace the example1 lines with commented versions
string(REPLACE "add_executable(example1 example1.cpp)" "# add_executable(example1 example1.cpp)" CNPY_CMAKE_CONTENT "${CNPY_CMAKE_CONTENT}")
string(REPLACE "target_link_libraries(example1 cnpy)" "# target_link_libraries(example1 cnpy)" CNPY_CMAKE_CONTENT "${CNPY_CMAKE_CONTENT}")

# Comment out find_package(ZLIB REQUIRED) to prevent conflict with CPM-managed ZLIB
string(REPLACE "find_package(ZLIB REQUIRED)" "# find_package(ZLIB REQUIRED)" CNPY_CMAKE_CONTENT "${CNPY_CMAKE_CONTENT}")

# Write the modified content back
file(WRITE "CMakeLists.txt" "${CNPY_CMAKE_CONTENT}")

message(STATUS "Successfully patched cnpy CMakeLists.txt")
