cmake_minimum_required(VERSION 3.21)
project(QuirewisePage VERSION 0.1.0 LANGUAGES C)
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
add_library(quirewise_page STATIC ${QW_SOURCES})
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
install(TARGETS quirewise_page EXPORT QuirewisePageTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewisePageTargets FILE QuirewisePageTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")
configure_package_config_file(cmake/QuirewisePageConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewisePageConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewisePage")
