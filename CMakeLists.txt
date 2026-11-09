cmake_minimum_required(VERSION 3.15)

project(Xenon
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal build by default so the plugin loads in both native Apple Silicon
# and Intel builds of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a faster single-arch dev build.
#
# This must sit immediately after project() and before any target is defined.
# CMake latches the architecture list when the first target is created, and
# setting it later is silently ignored -- you get an arm64-only binary that the
# build log calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and Resolume's plugin interface
# is still OpenGL, so every SDK call warns. Silencing it is not ignoring a
# problem -- there is no alternative API to move to here.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(NOMINMAX)
endif()

option(XENON_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule under external/.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The slice reader on its own: plain C++, no GL and no SDK, so slicedump and
# the harness's --reader check use exactly the code the plugin does.
# ---------------------------------------------------------------------------
add_library(xenon_slices OBJECT
    source/SliceMap.h
    source/SliceMap.cpp
)
target_include_directories(xenon_slices PUBLIC source)
set_target_properties(xenon_slices PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    # SHGetKnownFolderPath, for the Documents folder Arena keeps its presets in.
    target_link_libraries(xenon_slices PUBLIC shell32 ole32)
endif()

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. CFFGLPluginInfo registers itself from a
# file-scope constructor in PluginEntry.cpp and nothing ever references that
# object by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(xenon_core OBJECT
    source/Diag.h
    source/Diag.cpp
    source/Controls.h
    source/Controls.cpp
    source/Rig.h
    source/Rig.cpp
    source/Audio.h
    source/Audio.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/GLState.h
    source/Shaders.h
    source/Shaders.cpp
    source/Xenon.h
    source/Xenon.cpp
)

target_include_directories(xenon_core PUBLIC source)
# An OBJECT library's objects reach only the target that links it directly,
# never that target's consumers, so the bundle and the harness each name
# xenon_slices themselves below; this line carries its include path.
target_link_libraries(xenon_core PUBLIC ffgl::sdk xenon_slices)
target_compile_definitions(xenon_core PUBLIC XENON_VERSION="${PROJECT_VERSION}")
set_target_properties(xenon_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# On Windows/Linux the SDK headers we include pull in <GL/glew.h>. The SDK links
# GLEW privately, so consumers need it too for the include path (and for the
# GLEW_STATIC define the static vcpkg triplet propagates). macOS uses the system
# OpenGL framework and needs none of this.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(xenon_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin itself: a loadable macOS .bundle (MODULE library).
# ---------------------------------------------------------------------------
add_library(Xenon MODULE source/PluginEntry.cpp)
target_link_libraries(Xenon PRIVATE xenon_core xenon_slices)

if(APPLE)
    set_target_properties(Xenon PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        OUTPUT_NAME "Xenon"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "Xenon"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.stoatworks.ffgl.xenon"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
    target_link_libraries(Xenon PRIVATE
        "-framework OpenGL"
        "-framework Carbon"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# Offline harness. Whether the reader finds the operator's slices, whether a
# bank recharges as a capacitor does, whether a flash keeps its energy across
# frames, whether the chase steps where the beat fell and whether light lands
# inside a slice and nowhere else are measurements rather than matters of
# taste -- so this is where the effect is actually verified rather than merely
# previewed.
# ---------------------------------------------------------------------------
if(XENON_BUILD_TOOLS)
    add_executable(slicedump tools/slicedump/main.cpp)
    target_link_libraries(slicedump PRIVATE xenon_slices)
endif()

if(XENON_BUILD_TOOLS AND APPLE)
    add_executable(xntest tools/xntest/main.cpp)
    # zlib ships with the OS, which is why the PNG writer is fifty lines here
    # rather than a vendored dependency.
    # The committed presets the checks read: tests/presets.
    target_compile_definitions(xntest PRIVATE XENON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(xntest PRIVATE xenon_core xenon_slices z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()

# ---------------------------------------------------------------------------
# `cmake --install` drops the bundle straight into the user's Resolume plugin
# folder. Override with -DCMAKE_INSTALL_PREFIX=...
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # honour an explicit prefix
elseif(APPLE)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()

install(TARGETS Xenon
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)
