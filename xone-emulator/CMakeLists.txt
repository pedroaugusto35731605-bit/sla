cmake_minimum_required(VERSION 3.20)
project(XOneEmu VERSION 0.1.0 LANGUAGES CXX)

# -----------------------------------------------------------------------------
#  Opções
# -----------------------------------------------------------------------------
option(XONE_ENABLE_VULKAN "Compila o backend de renderização Vulkan" ON)
option(XONE_BUILD_TESTS   "Compila os testes do núcleo"              ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

# std::format exige GCC >= 13, Clang >= 17 (libc++) ou MSVC 19.29+
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "XOne-Emu precisa de GCC 13+ (std::format). Encontrado: ${CMAKE_CXX_COMPILER_VERSION}")
endif()

# -----------------------------------------------------------------------------
#  Biblioteca do núcleo
# -----------------------------------------------------------------------------
add_library(xone_core STATIC
  core/memory.cpp
  core/cpu.cpp
  core/jit_engine.cpp
  core/gpu.cpp
  core/bios.cpp
  filesystem/iso_parser.cpp
  filesystem/content_store.cpp
)
target_include_directories(xone_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/filesystem)

find_package(Threads REQUIRED)
target_link_libraries(xone_core PUBLIC Threads::Threads)

if(XONE_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    message(STATUS "XOne-Emu: backend Vulkan habilitado (${Vulkan_LIBRARIES})")
    target_link_libraries(xone_core PUBLIC Vulkan::Vulkan)
    target_compile_definitions(xone_core PUBLIC XONE_HAS_VULKAN=1)
  else()
    message(WARNING "XOne-Emu: Vulkan SDK não encontrado — compilando com GPU headless. "
                    "Instale o Vulkan SDK (Windows) ou libvulkan-dev (Linux).")
  endif()
endif()

if(MSVC)
  target_compile_options(xone_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(xone_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(xone_core PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# -----------------------------------------------------------------------------
#  Executável
# -----------------------------------------------------------------------------
add_executable(xone-emu core/main.cpp)
target_link_libraries(xone-emu PRIVATE xone_core)
if(MSVC)
  target_compile_options(xone-emu PRIVATE /utf-8)
endif()

# -----------------------------------------------------------------------------
#  Testes
# -----------------------------------------------------------------------------
if(XONE_BUILD_TESTS)
  enable_testing()
  add_executable(xone_tests tests/core_tests.cpp)
  target_link_libraries(xone_tests PRIVATE xone_core)
  if(MSVC)
    target_compile_options(xone_tests PRIVATE /utf-8)
  endif()
  add_test(NAME core_tests COMMAND xone_tests)
  add_test(NAME boot_interpreter COMMAND xone-emu boot --quiet)
  add_test(NAME boot_jit         COMMAND xone-emu boot --quiet --jit)
endif()

install(TARGETS xone-emu RUNTIME DESTINATION bin)
