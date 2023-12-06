# These are required in every CMakeLists.txt file
cmake_minimum_required(VERSION 2.8.9)
project(project_name)

# This is the name of the final executable
set(EXE_NAME load_bitstream)

# Use the C++17 language standard
set (CMAKE_CXX_STANDARD 17)

# Get a list of all the source files used for the executable application
file(GLOB SOURCES src/*.cpp)

# Specify what source files our executable is built from
add_executable(${EXE_NAME} ${SOURCES})

# And our executable statically links in these libraries
target_link_libraries(${EXE_NAME} pthread)

# After the build, strip debug symbols from the target
add_custom_command(
  TARGET ${EXE_NAME} POST_BUILD
  COMMAND strip ${EXE_NAME}
  VERBATIM
)
