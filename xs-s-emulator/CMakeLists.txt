cmake_minimum_required(VERSION 3.20)
project(SeriesSEmu VERSION 0.1.0 LANGUAGES CXX)

# -----------------------------------------------------------------------------
#  Opções
# -----------------------------------------------------------------------------
option(XSS_ENABLE_VULKAN "Compila o backend de renderização Vulkan"                       ON)
option(XSS_NATIVE_ZEN2   "Otimiza para CPUs AMD Zen 2 (-march=znver2). Desligue para binários portáveis" ON)
option(XSS_ENABLE_LTO    "Link-time optimization no build Release"                         ON)
option(XSS_BUILD_TESTS   "Compila os testes do núcleo"                                     ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de build" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "SeriesS-Emu precisa de GCC 13+ (std::format). Encontrado: ${CMAKE_CXX_COMPILER_VERSION}")
endif()

# -----------------------------------------------------------------------------
#  Flags de otimização
#   GCC/Clang: -O3 + -march=znver2 -mtune=znver2 (AVX2, BMI2, SHA, CLZERO do Zen 2)
#   MSVC     : /O2 /Ob3 + /arch:AVX2 (o MSVC não tem alvo específico para Zen 2)
#  ATENÇÃO: binários com -march=znver2 usam instruções que CPUs antigas (ou Intel
#  sem AVX2) não têm. Para distribuir, configure com -DXSS_NATIVE_ZEN2=OFF.
# -----------------------------------------------------------------------------
set(XSS_OPT_FLAGS "")
if(MSVC)
  list(APPEND XSS_OPT_FLAGS $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Ob3 /Oi /GL>)
  if(XSS_NATIVE_ZEN2)
    list(APPEND XSS_OPT_FLAGS /arch:AVX2)
  endif()
else()
  list(APPEND XSS_OPT_FLAGS $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
  if(XSS_NATIVE_ZEN2)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=znver2" XSS_HAS_ZNVER2)
    if(XSS_HAS_ZNVER2)
      list(APPEND XSS_OPT_FLAGS -march=znver2 -mtune=znver2)
      message(STATUS "SeriesS-Emu: otimizando para AMD Zen 2 (-O3 -march=znver2)")
    else()
      message(WARNING "SeriesS-Emu: compilador sem -march=znver2 (host ARM?) — usando -O3 genérico")
    endif()
  endif()
endif()

if(XSS_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT XSS_IPO_OK OUTPUT XSS_IPO_MSG)
  if(XSS_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

# -----------------------------------------------------------------------------
#  Biblioteca do núcleo
# -----------------------------------------------------------------------------
add_library(xss_core STATIC
  core/memory_manager.cpp
  core/codec.cpp
  core/ssd_io.cpp
  core/cpu.cpp
  core/jit_engine.cpp
  core/gpu.cpp
  core/bios_uefi.cpp
  filesystem/iso_parser.cpp
  filesystem/texture_streamer.cpp
  filesystem/image_builder.cpp
)
target_include_directories(xss_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/filesystem)
target_compile_options(xss_core PUBLIC ${XSS_OPT_FLAGS})

find_package(Threads REQUIRED)
target_link_libraries(xss_core PUBLIC Threads::Threads)

if(XSS_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    message(STATUS "SeriesS-Emu: backend Vulkan habilitado (${Vulkan_LIBRARIES})")
    target_link_libraries(xss_core PUBLIC Vulkan::Vulkan)
    target_compile_definitions(xss_core PUBLIC XSS_HAS_VULKAN=1)
  else()
    message(WARNING "SeriesS-Emu: Vulkan SDK não encontrado — GPU headless. "
                    "Instale o Vulkan SDK (Windows) ou libvulkan-dev (Linux).")
  endif()
endif()

if(MSVC)
  target_compile_options(xss_core PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_definitions(xss_core PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_options(xss_core PUBLIC $<$<CONFIG:Release,RelWithDebInfo>:/LTCG>)
else()
  target_compile_options(xss_core PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()

# -----------------------------------------------------------------------------
#  Executável e testes
# -----------------------------------------------------------------------------
add_executable(xss-emu core/main.cpp)
target_link_libraries(xss-emu PRIVATE xss_core)
if(MSVC)
  target_compile_options(xss-emu PRIVATE /utf-8)
endif()

if(XSS_BUILD_TESTS)
  enable_testing()
  add_executable(xss_tests tests/core_tests.cpp)
  target_link_libraries(xss_tests PRIVATE xss_core)
  if(MSVC)
    target_compile_options(xss_tests PRIVATE /utf-8)
  endif()
  add_test(NAME core_tests COMMAND xss_tests)
  add_test(NAME boot_interpreter COMMAND xss-emu boot --quiet)
  add_test(NAME boot_jit         COMMAND xss-emu boot --quiet --jit)
  set_tests_properties(boot_interpreter boot_jit PROPERTIES ENVIRONMENT "SERIESS_LOG=quiet")
endif()

install(TARGETS xss-emu RUNTIME DESTINATION bin)
