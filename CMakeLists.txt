cmake_minimum_required(VERSION 3.21)
project(QuirewiseText VERSION 0.1.0 LANGUAGES C)
option(QUIREWISE_BUILD_SHARED "Build Quirewise libraries as shared libraries" OFF)
if(QUIREWISE_BUILD_SHARED)
    set(QW_LIBRARY_TYPE SHARED)
else()
    set(QW_LIBRARY_TYPE STATIC)
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(QuirewiseCharset CONFIG REQUIRED)
file(GLOB_RECURSE QW_SOURCES CONFIGURE_DEPENDS src/*.c src/*.cpp)
add_library(quirewise_text ${QW_LIBRARY_TYPE} ${QW_SOURCES})
add_library(Quirewise::Text ALIAS quirewise_text)
set_target_properties(quirewise_text PROPERTIES EXPORT_NAME Text POSITION_INDEPENDENT_CODE ON)
target_compile_features(quirewise_text PUBLIC c_std_11)
target_link_libraries(quirewise_text PUBLIC Quirewise::Charset)
target_compile_definitions(quirewise_text PRIVATE QUIREWISE_STANDALONE_PLAIN_TEXT_PARSER=1)
target_include_directories(quirewise_text PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/formats ${CMAKE_CURRENT_SOURCE_DIR}/src/backends)
install(TARGETS quirewise_text EXPORT QuirewiseTextTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT QuirewiseTextTargets FILE QuirewiseTextTargets.cmake NAMESPACE Quirewise:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseText")
configure_package_config_file(cmake/QuirewiseTextConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseTextConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseText")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/QuirewiseTextConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseTextConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/QuirewiseTextConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/QuirewiseText")

if(QUIREWISE_BUILD_SHARED AND WIN32)
    set_target_properties(
        quirewise_text
        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(quirewise_text_fuzz_smoke tests/text_fuzz_smoke.c)
    target_compile_features(quirewise_text_fuzz_smoke PRIVATE c_std_11)
    target_link_libraries(quirewise_text_fuzz_smoke PRIVATE Quirewise::Text)
    add_test(NAME quirewise_text_fuzz_smoke
        COMMAND quirewise_text_fuzz_smoke)
endif()
