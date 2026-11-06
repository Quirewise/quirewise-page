cmake_minimum_required(VERSION 3.21)
project(QuirewisePage VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET Quirewise::Image)
    find_package(QuirewiseImage CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Archive)
    find_package(QuirewiseArchive CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Crypto)
    find_package(QuirewiseCrypto CONFIG REQUIRED)
endif()
if(NOT TARGET Quirewise::Compression)
    find_package(QuirewiseCompression CONFIG REQUIRED)
endif()
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_page ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Page ALIAS quirewise_page)
set_target_properties(quirewise_page PROPERTIES EXPORT_NAME Page POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_page PUBLIC c_std_11)
target_include_directories(quirewise_page PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
target_link_libraries(quirewise_page PRIVATE
    Quirewise::Image Quirewise::Archive Quirewise::Crypto
    Quirewise::Compression)
# PDF/CAJ geometry needs libm; PUBLIC so static consumers inherit it and the
# shared library records a real DT_NEEDED instead of exporting undefined
# references.
target_link_libraries(quirewise_page PUBLIC
    "$<$<NOT:$<PLATFORM_ID:Windows>>:m>")
install(TARGETS quirewise_page EXPORT QuirewisePageTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewisePageTargets FILE QuirewisePageTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")
configure_package_config_file(cmake/QuirewisePageConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_page
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(quirewise_page_fuzz_smoke tests/page_fuzz_smoke.c)
    target_compile_features(quirewise_page_fuzz_smoke PRIVATE c_std_11)
    target_link_libraries(quirewise_page_fuzz_smoke PRIVATE Quirewise::Page)
    add_test(NAME quirewise_page_fuzz_smoke
        COMMAND quirewise_page_fuzz_smoke)

    add_executable(quirewise_page_djvu_decode_test tests/djvu_decode_test.c)
    target_compile_features(quirewise_page_djvu_decode_test PRIVATE c_std_11)
    target_compile_definitions(quirewise_page_djvu_decode_test PRIVATE
        QUIREWISE_DJVU_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/djvu")
    target_link_libraries(quirewise_page_djvu_decode_test PRIVATE Quirewise::Page)
    add_test(NAME quirewise_page_djvu_decode
        COMMAND quirewise_page_djvu_decode_test)
endif()
