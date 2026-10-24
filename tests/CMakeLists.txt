# Floar unit tests (enabled with -DFLOAR_BUILD_TESTS=ON). Floar can be
# configured on its own (cmake -S Floar -B build-floar -DFLOAR_BUILD_TESTS=ON)
# or as part of the FICture2 tree. Vendored zlib/xz/libarchive tests stay off.

add_executable(FloarTests
    TestHarness.h
    TestMain.cpp
    LogTests.cpp
)

target_include_directories(FloarTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(FloarTests PRIVATE
    UNICODE
    _UNICODE
)

target_link_libraries(FloarTests PRIVATE
    Floar
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
# Safety net only: the tests contain no unbounded waits.
set_tests_properties(Floar.Log PROPERTIES TIMEOUT 120)
