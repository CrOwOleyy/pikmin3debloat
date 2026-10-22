cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(DecafGpu C CXX) # build autonome (test) ; sinon inclus par ../CMakeLists.txt
endif()
# Moteur de rendu : libgpu de decaf-emu (PM4 -> Vulkan, shaders Latte -> SPIR-V, détuilage)
# compilée telle quelle depuis tools/decaf-emu, avec une petite cale à la place de libcpu
# (cpu_shim.cpp). ⚠️ decaf-emu est sous GPL-3.0 : un exécutable qui l'intègre l'est aussi.
set(DECAF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/decaf-emu)
set(DECAF_LIBS ${DECAF_DIR}/libraries)
set(DECAF_SRC ${DECAF_DIR}/src)

find_package(Vulkan REQUIRED)
# En-têtes Vulkan de l'époque de decaf (nov. 2023) : le vulkan.hpp du SDK récent a déplacé
# des types (vk::detail) et changé des signatures. Seul le loader vulkan-1.lib vient du SDK.
set(DECAF_VULKAN_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/Vulkan-Headers-1.3.268/include)

# Options communes aux cibles decaf (reprises de son CMakeLists racine).
set(DECAF_DEFINITIONS
    NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    DECAF_VULKAN VK_USE_PLATFORM_WIN32_KHR VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0
    GSL_THROWS_FOR_TESTING)
set(DECAF_OPTIONS -march=x86-64-v3 /EHsc -Wno-everything /Z7)

# ---------------------------------------------------------------- bibliothèques tierces
add_library(decaf_addrlib STATIC
    ${DECAF_LIBS}/addrlib/src/addrinterface.cpp
    ${DECAF_LIBS}/addrlib/src/core/addrelemlib.cpp
    ${DECAF_LIBS}/addrlib/src/core/addrlib.cpp
    ${DECAF_LIBS}/addrlib/src/core/addrobject.cpp
    ${DECAF_LIBS}/addrlib/src/r600/r600addrlib.cpp)
target_include_directories(decaf_addrlib PRIVATE ${DECAF_LIBS}/addrlib/src PUBLIC ${DECAF_LIBS}/addrlib/include)
target_compile_options(decaf_addrlib PRIVATE ${DECAF_OPTIONS})

add_subdirectory(${DECAF_LIBS}/fmt fmt EXCLUDE_FROM_ALL)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
add_subdirectory(${DECAF_LIBS}/spdlog spdlog EXCLUDE_FROM_ALL)

# glslang : seul spv::Builder sert (le traducteur Latte -> SPIR-V en hérite), sans optimiseur.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${DECAF_LIBS}/glslang glslang EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- decaf : common
file(GLOB DECAF_COMMON_SOURCES ${DECAF_SRC}/common/src/*.cpp ${DECAF_SRC}/common/*.cpp) # xxhash.c est inclus par datahash.h
add_library(decaf_common STATIC ${DECAF_COMMON_SOURCES})
target_include_directories(decaf_common BEFORE PUBLIC ${DECAF_VULKAN_HEADERS})
target_include_directories(decaf_common PUBLIC
    ${DECAF_SRC} ${DECAF_SRC}/common
    ${DECAF_LIBS}/gsl-lite/include ${DECAF_LIBS}/cnl/include)
target_compile_definitions(decaf_common PUBLIC ${DECAF_DEFINITIONS})
target_compile_options(decaf_common PRIVATE ${DECAF_OPTIONS})
target_link_libraries(decaf_common PUBLIC fmt::fmt spdlog::spdlog Dbghelp)

# ---------------------------------------------------------------- decaf : libgpu
set(LIBGPU_DIR ${DECAF_SRC}/libgpu)
set(LIBGPU_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_program(GLSLANG_VALIDATOR NAMES glslangValidator HINTS "${Vulkan_INCLUDE_DIR}/../Bin" REQUIRED)

# Shader de détuilage compilé en SPIR-V puis embarqué en C (comme dans decaf).
set(TILING_SPV ${LIBGPU_GENERATED}/vulkan_shaders_bin/gpu7_tiling.comp.spv)
add_custom_command(
    OUTPUT ${TILING_SPV}.h ${TILING_SPV}.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LIBGPU_GENERATED}/vulkan_shaders_bin
    COMMAND ${GLSLANG_VALIDATOR} -V -o ${TILING_SPV} ${LIBGPU_DIR}/vulkan_shaders/gpu7_tiling.comp.glsl
    COMMAND ${CMAKE_COMMAND} -DOUTPUT_C=${TILING_SPV}.cpp -DOUTPUT_H=${TILING_SPV}.h -DINPUT_FILES=${TILING_SPV}
            -P ${DECAF_LIBS}/bin2c.cmake
    DEPENDS ${LIBGPU_DIR}/vulkan_shaders/gpu7_tiling.comp.glsl
    VERBATIM)

file(GLOB_RECURSE LIBGPU_SOURCES ${LIBGPU_DIR}/*.cpp)
add_library(decaf_gpu STATIC ${LIBGPU_SOURCES} ${TILING_SPV}.cpp cpu_shim.cpp)
target_include_directories(decaf_gpu
    PUBLIC ${DECAF_SRC} ${LIBGPU_DIR}
    PRIVATE ${LIBGPU_DIR}/src ${LIBGPU_DIR}/vulkan_shaders ${LIBGPU_GENERATED}
            ${DECAF_LIBS}/glslang)
# decaf comptait sur des inclusions transitives de son en-tête précompilé (<chrono>…).
target_compile_options(decaf_gpu PRIVATE ${DECAF_OPTIONS} /FIchrono)
target_link_libraries(decaf_gpu PUBLIC decaf_common decaf_addrlib Vulkan::Vulkan SPIRV)

# decaf compile en C++17 hors MSVC : en C++20, fmt vérifie les chaînes de format à la
# compilation (consteval), ce que le logger de decaf (chaînes passées en paramètre) ne permet pas.
set_target_properties(fmt spdlog decaf_common decaf_gpu PROPERTIES CXX_STANDARD 17)

# ---------------------------------------------------------------- decaf : GX2 (HLE) + libgfd
# Sources GX2 de decaf compilées telles quelles ; ce qu'elles appellent hors de GX2
# (coreinit, TCL, invocation PPC…) est fourni par compat/ au-dessus de notre runtime.
set(LIBDECAF_SRC ${DECAF_SRC}/libdecaf/src)
file(GLOB DECAF_GX2_SOURCES ${LIBDECAF_SRC}/cafe/libraries/gx2/*.cpp ${DECAF_SRC}/libgfd/src/*.cpp)
add_library(decaf_gx2 STATIC ${DECAF_GX2_SOURCES})
target_include_directories(decaf_gx2 PUBLIC
    ${LIBDECAF_SRC} ${DECAF_SRC}/libdecaf ${DECAF_SRC}/libgfd ${DECAF_SRC})
target_compile_options(decaf_gx2 PRIVATE ${DECAF_OPTIONS} /FIchrono)
target_link_libraries(decaf_gx2 PUBLIC decaf_gpu)
set_target_properties(decaf_gx2 PROPERTIES CXX_STANDARD 17)

# ---------------------------------------------------------------- outil : latte-assembler
# Assembleur de shaders Latte de decaf (texte → .gsh), pour reconstruire lyt_BuildinShader.gsh
# (shaders/). Hors de la cible par défaut : cmake --build build --target latte_assembler
set(LATTE_ASM_DIR ${DECAF_DIR}/tools/latte-assembler)
file(GLOB LATTE_ASM_SOURCES ${LATTE_ASM_DIR}/src/*.cpp ${DECAF_SRC}/libgfd/src/*.cpp)
set(LATTE_ASM_GRAMMAR_H ${CMAKE_CURRENT_BINARY_DIR}/latte_asm/grammar.h)
file(READ ${LATTE_ASM_DIR}/resources/grammar.txt LATTE_ASM_GRAMMAR)
file(WRITE ${LATTE_ASM_GRAMMAR_H} "const char *LatteGrammar = R\"(\n${LATTE_ASM_GRAMMAR})\";\n")
add_executable(latte_assembler EXCLUDE_FROM_ALL ${LATTE_ASM_SOURCES})
target_include_directories(latte_assembler PRIVATE ${LATTE_ASM_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/latte_asm
    ${DECAF_SRC}/libgfd ${DECAF_SRC}/libdecaf/src ${DECAF_SRC}/libdecaf ${DECAF_LIBS}/excmd/src ${DECAF_LIBS}/cpp-peglib
    ${DECAF_LIBS}/glslang)
target_compile_options(latte_assembler PRIVATE ${DECAF_OPTIONS} /FIchrono)
target_link_libraries(latte_assembler PRIVATE decaf_gpu)
set_target_properties(latte_assembler PROPERTIES CXX_STANDARD 17)
