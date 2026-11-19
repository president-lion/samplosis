# The macOS build: Samplosis as CLAP, VST3 and Audio Unit (v2), from one static library through clap-wrapper.
# Windows keeps build.bat; this file is for the Mac.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# The plugins land in build/samplosis_assets (Samplosis.clap, Samplosis.vst3, Samplosis.component); install.command
# copies them to ~/Library/Audio/Plug-Ins. clap-wrapper, the CLAP headers, the VST3 SDK and Apple's AudioUnitSDK are
# downloaded at configure time.
cmake_minimum_required(VERSION 3.21)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS the plugins run on")
option(SMP_UNIVERSAL "Build for Apple silicon and Intel Macs" ON)
if (SMP_UNIVERSAL AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
project(samplosis VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

if (NOT APPLE)
  message(FATAL_ERROR "CMakeLists.txt is the macOS build; on Windows use build.bat")
endif()
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OBJC_VISIBILITY_PRESET hidden)

include(FetchContent)
FetchContent_Declare(clap-wrapper
  GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
  GIT_TAG v0.18.0
  GIT_SHALLOW TRUE)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(clap-wrapper)

set(S ${CMAKE_SOURCE_DIR}/src)
set(CO ${S}/codecs)

# ---- the vendored decoders (BSD and MIT), warnings off as in build.bat
file(GLOB OGG_SRC ${CO}/ogg/src/*.c)
file(GLOB OPUS_SRC ${CO}/opus/src/*.c ${CO}/opus/celt/*.c ${CO}/opus/silk/*.c)
file(GLOB OPUSFILE_SRC ${CO}/opusfile/src/*.c)
file(GLOB WAVPACK_SRC ${CO}/wavpack/src/*.c)
file(GLOB APE_SRC ${CO}/mac/Source/MACLib/*.cpp ${CO}/mac/Source/MACLib/Old/*.cpp ${CO}/mac/Source/Shared/*.cpp)
file(GLOB GIG_SRC ${CO}/libgig/RIFF.cpp ${CO}/libgig/DLS.cpp ${CO}/libgig/gig.cpp ${CO}/libgig/helper.cpp
                  ${CO}/libgig/Serialization.cpp ${CO}/libgig/srx/*.cpp)

add_library(smp_codecs STATIC ${OGG_SRC} ${OPUS_SRC} ${OPUSFILE_SRC} ${WAVPACK_SRC} ${APE_SRC} ${GIG_SRC}
                              ${CO}/yxml/yxml.c ${CO}/miniz/miniz.c)
target_include_directories(smp_codecs PUBLIC ${CO}/ogg/include ${CO}/opus/include ${CO}/opusfile/include
                                              ${CO}/wavpack/include ${CO}/yxml ${CO}/miniz ${CO}/libgig
                                       PRIVATE ${CO}/opus/celt ${CO}/opus/silk ${CO}/mac/Source/Shared ${CO}/mac/Source/MACLib)
target_compile_options(smp_codecs PRIVATE -w)
set_source_files_properties(${OPUS_SRC} PROPERTIES COMPILE_DEFINITIONS "OPUS_BUILD;USE_ALLOCA")
set_source_files_properties(${WAVPACK_SRC} PROPERTIES COMPILE_DEFINITIONS "ENABLE_DSD;ENABLE_LEGACY")
set_source_files_properties(${APE_SRC} PROPERTIES COMPILE_DEFINITIONS "PLATFORM_APPLE")
set_source_files_properties(${GIG_SRC} PROPERTIES COMPILE_DEFINITIONS "PACKAGE=\"libgig\";VERSION=\"4.6.0\"")
set_source_files_properties(${CO}/miniz/miniz.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_ZLIB_COMPATIBLE_NAMES;MINIZ_NO_ARCHIVE_WRITING_APIS")

# ---- the sampler and SoundFont player: everything but the plugin glue and the window
file(GLOB CORE_C ${S}/*.c)
list(REMOVE_ITEM CORE_C ${S}/plugin.c ${S}/gui.c)
add_library(smp_core STATIC ${CORE_C} ${S}/rb.cpp ${S}/stretch.cpp ${S}/gigshim.cpp ${CO}/mac_shim.cpp)
target_include_directories(smp_core PUBLIC ${S})
target_link_libraries(smp_core PUBLIC smp_codecs clap "-framework AudioToolbox" "-framework CoreFoundation"
                                      "-framework Accelerate")
target_compile_options(smp_core PRIVATE -Wall -Wno-unused-function -Wno-missing-braces -Wno-bitwise-instead-of-logical)
set_source_files_properties(${S}/rb.cpp ${S}/stretch.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${S}/gigshim.cpp PROPERTIES COMPILE_DEFINITIONS "PACKAGE=\"libgig\";VERSION=\"4.6.0\"")
set_source_files_properties(${CO}/mac_shim.cpp PROPERTIES COMPILE_DEFINITIONS "PLATFORM_APPLE"
                                                          INCLUDE_DIRECTORIES "${CO}/mac/Source/Shared;${CO}/mac/Source/MACLib")
set_source_files_properties(${S}/sfds.c ${S}/sfdslib.c PROPERTIES
  COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_ZLIB_COMPATIBLE_NAMES;MINIZ_NO_ARCHIVE_WRITING_APIS")

# ---- the plugin: CLAP glue and the Cocoa window, made into CLAP, VST3 and AU by clap-wrapper
add_library(samplosis_impl STATIC ${S}/plugin.c ${S}/mac/gui_mac.m)
target_compile_definitions(samplosis_impl PRIVATE SMP_CLAP_FIRST)
target_compile_options(samplosis_impl PRIVATE -Wall -Wno-unused-function)
set_source_files_properties(${S}/mac/gui_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(samplosis_impl PUBLIC smp_core clap "-framework Cocoa" "-framework UniformTypeIdentifiers")

make_clapfirst_plugins(
  TARGET_NAME samplosis
  IMPL_TARGET samplosis_impl
  OUTPUT_NAME "Samplosis"
  ENTRY_SOURCE ${S}/mac/entry.c
  BUNDLE_IDENTIFIER "com.audioskit.samplosis"
  BUNDLE_VERSION ${PROJECT_VERSION}
  COPY_AFTER_BUILD FALSE
  PLUGIN_FORMATS CLAP VST3 AUV2
  ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/samplosis_assets
  AUV2_MANUFACTURER_NAME "audioskit"
  AUV2_MANUFACTURER_CODE "AuSk"
  AUV2_SUBTYPE_CODE "Smps"
  AUV2_INSTRUMENT_TYPE "aumu")

# ---- a command-line check of the engine on the Mac (tests/mac_smoke.c)
add_executable(smp_smoke ${CMAKE_SOURCE_DIR}/tests/mac_smoke.c)
target_link_libraries(smp_smoke PRIVATE smp_core)
