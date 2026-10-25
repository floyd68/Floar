# Floar unit tests (enabled with -DFLOAR_BUILD_TESTS=ON). Floar can be
# configured on its own (cmake -S Floar -B build-floar -DFLOAR_BUILD_TESTS=ON)
# or as part of the FICture2 tree. Vendored zlib/xz/libarchive tests stay off.

add_executable(FloarTests
    TestArchives.h
    TestHarness.h
    TestMain.cpp
    Ba2Tests.cpp
    LogTests.cpp
    OpenPoolTests.cpp
    StreamingTests.cpp
    VfsTests.cpp
)

target_include_directories(FloarTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # zlib builds the BA2 fixtures' compressed chunks.
    ${PROJECT_SOURCE_DIR}/external/zlib
    ${PROJECT_BINARY_DIR}/external/zlib
)

target_compile_definitions(FloarTests PRIVATE
    UNICODE
    _UNICODE
)

target_link_libraries(FloarTests PRIVATE
    Floar
    zlibstatic
)

if(FLOAR_ENABLE_COMMON_ARCHIVES)
    # The tests build their ZIP fixtures with libarchive's writer.
    target_include_directories(FloarTests PRIVATE
        ${PROJECT_SOURCE_DIR}/external/libarchive/libarchive
    )
    target_compile_definitions(FloarTests PRIVATE LIBARCHIVE_STATIC)
endif()

if(MSVC)
    target_compile_options(FloarTests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME Floar.Log COMMAND FloarTests Log_)
add_test(NAME Floar.Vfs COMMAND FloarTests Vfs)
add_test(NAME Floar.Ba2 COMMAND FloarTests Ba2_)
add_test(NAME Floar.Streaming COMMAND FloarTests Stream)
add_test(NAME Floar.OpenPool COMMAND FloarTests Pool_)
# Safety net only: the tests contain no unbounded waits.
set_tests_properties(Floar.Log Floar.Vfs Floar.Ba2 Floar.Streaming Floar.OpenPool PROPERTIES TIMEOUT 120)
