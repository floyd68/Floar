cmake_minimum_required(VERSION 3.21)

project(Floar LANGUAGES C CXX)

option(FLOAR_ENABLE_COMMON_ARCHIVES "Enable ZIP/7z/RAR via libarchive" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(floar_set_cache_bool name value description)
    set(${name} ${value} CACHE BOOL "${description}" FORCE)
endfunction()

function(floar_set_cache_string name value description)
    set(${name} ${value} CACHE STRING "${description}" FORCE)
endfunction()

function(floar_set_cache_path name value description)
    set(${name} ${value} CACHE PATH "${description}" FORCE)
endfunction()

function(floar_set_cache_bools_from_pairs pairs_list description_prefix)
    foreach(entry IN LISTS ${pairs_list})
        string(REPLACE "=" ";" parts "${entry}")
        list(LENGTH parts parts_len)
        if(parts_len LESS 2)
            message(FATAL_ERROR "Expected NAME=VALUE pair, got: ${entry}")
        endif()
        list(GET parts 0 name)
        list(GET parts 1 value)
        floar_set_cache_bool(${name} ${value} "${description_prefix} ${name}")
    endforeach()
endfunction()

function(floar_set_cache_bools_off names_list description_prefix)
    foreach(name IN LISTS ${names_list})
        floar_set_cache_bool(${name} OFF "${description_prefix} ${name}")
    endforeach()
endfunction()

# zlib (BA2 support)
floar_set_cache_bool(ZLIB_BUILD_SHARED OFF "Disable zlib shared")
floar_set_cache_bool(ZLIB_BUILD_STATIC ON "Enable zlib static")
floar_set_cache_bool(ZLIB_BUILD_TESTING OFF "Disable zlib tests")
floar_set_cache_bool(ZLIB_INSTALL OFF "Disable zlib install")
add_subdirectory(external/zlib)
floar_set_cache_string(ZLIB_LIBRARY zlibstatic "Use local zlib")
floar_set_cache_string(
    ZLIB_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/external/zlib;${CMAKE_CURRENT_BINARY_DIR}/external/zlib"
    "Use local zlib includes"
)

# libarchive + xz (ZIP/7z/RAR)
if(FLOAR_ENABLE_COMMON_ARCHIVES)
    set(_floar_xz_bool_options
        XZ_NLS
        XZ_DOC
        XZ_DOXYGEN
        XZ_TOOL_XZ
        XZ_TOOL_XZDEC
        XZ_TOOL_LZMADEC
        XZ_TOOL_LZMAINFO
        XZ_TOOL_SCRIPTS
        XZ_TOOL_SYMLINKS
        XZ_TOOL_SYMLINKS_LZMA
    )
    floar_set_cache_bools_off(_floar_xz_bool_options "Disable xz")
    floar_set_cache_bool(BUILD_SHARED_LIBS OFF "Static xz")
    floar_set_cache_bool(XZ_SMALL ON "Reduce liblzma code/table size (decoder-only usage)")
    add_subdirectory(external/xz)

    set(_floar_liblzma_config_dir "${CMAKE_CURRENT_BINARY_DIR}/_liblzma_config")
    file(MAKE_DIRECTORY "${_floar_liblzma_config_dir}")
    file(WRITE "${_floar_liblzma_config_dir}/LibLZMAConfig.cmake"
        "set(LIBLZMA_FOUND TRUE)\n"
        "set(LIBLZMA_INCLUDE_DIRS \"${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api\")\n"
        "set(LIBLZMA_LIBRARIES liblzma)\n")
    file(WRITE "${_floar_liblzma_config_dir}/liblzma-config.cmake"
        "include(\"${_floar_liblzma_config_dir}/LibLZMAConfig.cmake\")\n")

    floar_set_cache_string(POSIX_REGEX_LIB LIBC "Use libc regex")
    set(_floar_libarchive_cache_bools
        ENABLE_WERROR=OFF
        ENABLE_MBEDTLS=OFF
        ENABLE_NETTLE=OFF
        ENABLE_OPENSSL=OFF
        ENABLE_LIBB2=OFF
        ENABLE_LZ4=OFF
        ENABLE_LZO=OFF
        ENABLE_LZMA=ON
        ENABLE_ZSTD=OFF
        ENABLE_ZLIB=ON
        ENABLE_BZip2=OFF
        ENABLE_LIBXML2=OFF
        ENABLE_EXPAT=OFF
        ENABLE_WIN32_XMLLITE=OFF
        ENABLE_PCREPOSIX=OFF
        ENABLE_PCRE2POSIX=OFF
        ENABLE_CNG=OFF
        ENABLE_TAR=OFF
        ENABLE_CPIO=OFF
        ENABLE_CAT=OFF
        ENABLE_UNZIP=OFF
        ENABLE_XATTR=OFF
        ENABLE_ACL=OFF
        ENABLE_ICONV=OFF
        ENABLE_TEST=OFF
        ENABLE_INSTALL=OFF
        BUILD_SHARED_LIBS=OFF
    )
    floar_set_cache_bools_from_pairs(_floar_libarchive_cache_bools "libarchive")

    floar_set_cache_path(LibLZMA_DIR "${_floar_liblzma_config_dir}" "Use local liblzma config")
    floar_set_cache_bool(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON "Prefer config packages")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_BINARY_DIR}/external/xz")
    floar_set_cache_string(
        LIBLZMA_INCLUDE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/external/xz/src/liblzma/api"
        "Use local liblzma include"
    )
    floar_set_cache_path(
        LIBLZMA_LIBRARY_RELEASE
        "${CMAKE_CURRENT_BINARY_DIR}/external/xz/Release/lzma.lib"
        "Use local liblzma release"
    )
    floar_set_cache_path(
        LIBLZMA_LIBRARY_DEBUG
        "${CMAKE_CURRENT_BINARY_DIR}/external/xz/Debug/lzma.lib"
        "Use local liblzma debug"
    )

    add_subdirectory(external/libarchive)
endif()

add_library(Floar STATIC)

target_sources(Floar PRIVATE
    ArchiveReader.cpp
    FloarLog.cpp
    VirtualFileSystem.cpp
    VirtualPath.cpp
)

target_include_directories(Floar
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/zlib
        ${CMAKE_CURRENT_BINARY_DIR}/external/zlib
)

target_compile_definitions(Floar
    PUBLIC
        $<$<BOOL:${FLOAR_ENABLE_COMMON_ARCHIVES}>:FLOAR_ENABLE_COMMON_ARCHIVES=1>
        $<$<NOT:$<BOOL:${FLOAR_ENABLE_COMMON_ARCHIVES}>>:FLOAR_ENABLE_COMMON_ARCHIVES=0>
    PRIVATE
        UNICODE
        _UNICODE
)

if(FLOAR_ENABLE_COMMON_ARCHIVES)
    target_include_directories(Floar PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libarchive/libarchive
    )
    target_compile_definitions(Floar PRIVATE LIBARCHIVE_STATIC)
    target_link_libraries(Floar PUBLIC archive_static)
else()
    target_link_libraries(Floar PUBLIC zlibstatic)
endif()

if(MSVC)
    target_compile_options(Floar PRIVATE /permissive-)
endif()
