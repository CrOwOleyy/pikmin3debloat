# pikmin3.exe : code de Pikmin 3 recompilé (generated/, produit par XenonRecomp) + runtime HLE.
#
#   python ../scripts/link_rpl.py ../extracted/carrot.elf carrot_linked.elf config/imports.toml runtime/generated/imports.inc
#   python ../scripts/gen_recomp_config.py ../extracted/carrot.elf carrot_linked.elf config/carrot_functions.toml config
#   ../tools/XenonRecomp/build-win/XenonRecomp/XenonRecomp.exe config/carrot.toml runtime/ppc_context.h
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build
cmake_minimum_required(VERSION 3.20)
project(Pikmin3Recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Le code généré par XenonRecomp exige clang (clang-cl sous Windows).")
endif()

set(SIMDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/XenonRecomp/thirdparty/simde)

file(GLOB GENERATED_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generated/ppc_recomp.*.cpp)
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cpp)

# Code recompilé : bibliothèque à part, en-tête partagé précompilé (3,4 Mo de déclarations).
add_library(carrot_recomp STATIC ${GENERATED_SOURCES} generated/ppc_func_mapping.cpp)
target_include_directories(carrot_recomp PUBLIC generated ${SIMDE_DIR})
target_precompile_headers(carrot_recomp PRIVATE generated/ppc_recomp_shared.h)
target_compile_options(carrot_recomp PRIVATE -march=x86-64-v3 -Wno-everything /O2)

add_executable(pikmin3 ${RUNTIME_SOURCES})
target_include_directories(pikmin3 PRIVATE runtime generated ${SIMDE_DIR})
target_compile_options(pikmin3 PRIVATE -march=x86-64-v3 /EHsc -Wno-unused-parameter -Wno-nan-infinity-disabled)
target_compile_definitions(pikmin3 PRIVATE
    PIKMIN3_RECOMP_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(pikmin3 PRIVATE carrot_recomp Xinput)

# Rendu : GX2 + libgpu de decaf-emu (gpu/) et le pont vers notre runtime (gpu/bridge/).
# ⚠️ decaf-emu est sous GPL-3.0 : pikmin3.exe l'est donc aussi s'il est distribué.
add_subdirectory(gpu)
file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gpu/bridge/*.cpp)
add_library(decaf_bridge OBJECT ${BRIDGE_SOURCES})
target_include_directories(decaf_bridge PRIVATE runtime generated ${SIMDE_DIR})
target_compile_options(decaf_bridge PRIVATE -march=x86-64-v3 /EHsc -Wno-everything /FIchrono /Z7)
target_link_libraries(decaf_bridge PRIVATE decaf_gx2)
set_target_properties(decaf_bridge PROPERTIES CXX_STANDARD 17)
target_link_libraries(pikmin3 PRIVATE decaf_bridge decaf_gx2)
target_link_options(pikmin3 PRIVATE /STACK:16777216 /errorlimit:0 /DEBUG)
