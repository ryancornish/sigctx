cmake_minimum_required(VERSION 3.16)

project(sigctx
  VERSION 1.0.0
  DESCRIPTION "Synthesize and resume Linux rt_sigreturn-compatible CPU contexts (x86-64)"
  HOMEPAGE_URL "https://github.com/ryancornish/sigctx"
  LANGUAGES C)

# This library is x86-64 Linux only. Catch a wrong target at configure time
# rather than with a wall of compile errors from the ABI header.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
   OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  message(FATAL_ERROR
    "sigctx targets x86-64 Linux only, got ${CMAKE_SYSTEM_NAME} / ${CMAKE_SYSTEM_PROCESSOR}")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BUILD_SHARED_LIBS  "Build sigctx as a shared library" ON)
option(SIGCTX_BUILD_EXAMPLES "Build the example programs" OFF)
option(SIGCTX_BUILD_TESTS    "Build the test suite"       OFF)

# ---- the library -----------------------------------------------------------
add_library(sigctx
  src/sigctx.c
  src/sigctx_intercept.c)
add_library(sigctx::sigctx ALIAS sigctx)

# The sources use GNU C extensions (inline asm, the naked and noreturn
# attributes), so they must be built in a gnu dialect, not strict ISO C.
# C_EXTENSIONS ON selects -std=gnu11 here. Raise C_STANDARD to 23 for gnu23.
set_target_properties(sigctx PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS ON
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(sigctx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_options(sigctx PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)

# Export only the documented public symbols and give them a version tag. This
# keeps internal helpers out of the shared object and gives Debian a stable,
# trackable symbol set. Applies to the shared build with a GNU-style linker.
if(BUILD_SHARED_LIBS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(_sigctx_map "${CMAKE_CURRENT_SOURCE_DIR}/sigctx.map")
  target_link_options(sigctx PRIVATE "LINKER:--version-script=${_sigctx_map}")
  set_target_properties(sigctx PROPERTIES LINK_DEPENDS "${_sigctx_map}")
endif()

# ---- install: library + headers --------------------------------------------
install(TARGETS sigctx
  EXPORT  sigctxTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT Runtime
          NAMELINK_COMPONENT Development
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          COMPONENT Development
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/sigctx
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  COMPONENT Development
  FILES_MATCHING PATTERN "*.h")

# ---- install: CMake package config so find_package(sigctx) works -----------
install(EXPORT sigctxTargets
  FILE      sigctxTargets.cmake
  NAMESPACE sigctx::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sigctx
  COMPONENT Development)

configure_package_config_file(
  cmake/sigctxConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/sigctxConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sigctx)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/sigctxConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/sigctxConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/sigctxConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sigctx
  COMPONENT Development)

# ---- install: pkg-config ----------------------------------------------------
configure_file(sigctx.pc.in "${CMAKE_CURRENT_BINARY_DIR}/sigctx.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sigctx.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  COMPONENT Development)

# ---- examples and tests (off by default, never part of the package) --------
if(SIGCTX_BUILD_EXAMPLES OR SIGCTX_BUILD_TESTS)
  find_package(Threads REQUIRED)
endif()

if(SIGCTX_BUILD_EXAMPLES)
  add_executable(preempt_demo examples/preempt_demo.c)
  set_target_properties(preempt_demo PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
  target_link_libraries(preempt_demo PRIVATE sigctx Threads::Threads)
endif()

if(SIGCTX_BUILD_TESTS)
  enable_language(CXX)
  enable_testing()

  add_executable(test_sigctx tests/test_sigctx.cpp)
  set_target_properties(test_sigctx PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_options(test_sigctx PRIVATE -mavx)
  target_link_libraries(test_sigctx PRIVATE sigctx Threads::Threads)
  add_test(NAME sigctx_tests COMMAND test_sigctx)

  add_executable(test_preempt_stress tests/test_preempt_stress.cpp)
  set_target_properties(test_preempt_stress PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_options(test_preempt_stress PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
  target_link_libraries(test_preempt_stress PRIVATE sigctx Threads::Threads)
  add_test(NAME sigctx_preempt_stress COMMAND test_preempt_stress)
endif()
