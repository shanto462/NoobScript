cmake_minimum_required(VERSION 3.20)
project(NoobScript VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

add_executable(noob
  lib/Builtins.cpp
  lib/Diag.cpp
  lib/Interpreter.cpp
  lib/Lexer.cpp
  lib/Parser.cpp
  lib/Runtime.cpp
  lib/Value.cpp
  tools/noob.cpp
)

target_include_directories(noob PRIVATE include)
target_include_directories(noob SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(noob PRIVATE
  ${LLVM_DEFINITIONS}
  NOOB_VERSION="${PROJECT_VERSION}"
)
target_compile_options(noob PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)

# Homebrew ships one shared libLLVM. Source builds usually have component libs.
if(TARGET LLVM)
  target_link_libraries(noob PRIVATE LLVM)
else()
  llvm_map_components_to_libnames(NOOB_LLVM_LIBS support lineeditor)
  target_link_libraries(noob PRIVATE ${NOOB_LLVM_LIBS})
endif()

# Keeps an installed noob able to find Homebrew's libLLVM.
set_target_properties(noob PROPERTIES INSTALL_RPATH "${LLVM_LIBRARY_DIR}")
install(TARGETS noob RUNTIME DESTINATION bin)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME golden
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/run_tests.py
            $<TARGET_FILE:noob> ${CMAKE_SOURCE_DIR}/test/cases)
  add_test(NAME cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/test/test_cli.py $<TARGET_FILE:noob>)
endif()

add_executable(runtime_test EXCLUDE_FROM_ALL lib/Runtime.cpp test/runtime_test.cpp)
target_include_directories(runtime_test PRIVATE include)
target_include_directories(runtime_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(runtime_test PRIVATE -fno-exceptions -fno-rtti)
