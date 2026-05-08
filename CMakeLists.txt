cmake_minimum_required(VERSION 3.10)
project(myth_tools)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Git REQUIRED)

add_executable(myth_extract tfl/myth_extract.cpp)
add_executable(myth_assemble tfl/myth_assemble.cpp)
add_executable(myth_mesh tfl/myth_mesh.cpp)
add_executable(myth_mesh_import tfl/myth_mesh_import.cpp)
add_executable(tag_dump myth2/tag_dump.cpp)
add_executable(extract_map myth2/extract_map.cpp)
add_executable(export_mesh myth2/export_mesh.cpp)
add_executable(export_water_mesh myth2/export_water_mesh.cpp)
add_executable(water_depth myth2/water_depth.cpp)
add_executable(mesh_import myth2/mesh_import.cpp)
add_executable(mesh_diff myth2/mesh_diff.cpp)
add_executable(mesh_dump myth2/mesh_dump.cpp)
add_executable(mesh_summary myth2/mesh_summary.cpp)
add_executable(normal_analyze myth2/normal_analyze.cpp)
add_executable(normal_compare myth2/normal_compare.cpp)
add_executable(normal_table myth2/normal_table.cpp)
add_executable(media_height myth2/media_height.cpp)
add_executable(media_dump myth2/media_dump.cpp)
add_executable(core_dump myth2/core_dump.cpp)
add_executable(proj_dump myth2/proj_dump.cpp)
add_executable(build_plugin myth2/build_plugin.cpp)
add_executable(export_models myth2/export_models.cpp)

set(RELEASE_TARGETS
    myth_extract
    myth_assemble
    myth_mesh
    myth_mesh_import
    tag_dump
    extract_map
    export_mesh
    export_water_mesh
    water_depth
    mesh_import
    mesh_diff
    mesh_dump
    mesh_summary
    normal_analyze
    normal_compare
    normal_table
    media_height
    media_dump
    core_dump
    proj_dump
    build_plugin
    export_models
)

# Enable warnings
if (MSVC)
    target_compile_options(myth_extract PRIVATE /W3)
    target_compile_options(myth_assemble PRIVATE /W3)
    target_compile_options(myth_mesh PRIVATE /W3)
    target_compile_options(myth_mesh_import PRIVATE /W3)
    target_compile_options(tag_dump PRIVATE /W3)
    target_compile_options(extract_map PRIVATE /W3)
    target_compile_options(export_mesh PRIVATE /W3)
    target_compile_options(export_water_mesh PRIVATE /W3)
    target_compile_options(water_depth PRIVATE /W3)
    target_compile_options(mesh_import PRIVATE /W3)
    target_compile_options(mesh_diff PRIVATE /W3)
    target_compile_options(mesh_dump PRIVATE /W3)
    target_compile_options(mesh_summary PRIVATE /W3)
    target_compile_options(normal_analyze PRIVATE /W3)
    target_compile_options(normal_compare PRIVATE /W3)
    target_compile_options(normal_table PRIVATE /W3)
    target_compile_options(media_height PRIVATE /W3)
    target_compile_options(media_dump PRIVATE /W3)
    target_compile_options(core_dump PRIVATE /W3)
    target_compile_options(proj_dump PRIVATE /W3)
    target_compile_options(build_plugin PRIVATE /W3)
    target_compile_options(export_models PRIVATE /W3)
else()
    target_compile_options(myth_extract PRIVATE -Wall -Wextra)
    target_compile_options(myth_assemble PRIVATE -Wall -Wextra)
    target_compile_options(myth_mesh PRIVATE -Wall -Wextra)
    target_compile_options(myth_mesh_import PRIVATE -Wall -Wextra)
    target_compile_options(tag_dump PRIVATE -Wall -Wextra)
    target_compile_options(extract_map PRIVATE -Wall -Wextra)
    target_compile_options(export_mesh PRIVATE -Wall -Wextra)
    target_compile_options(export_water_mesh PRIVATE -Wall -Wextra)
    target_compile_options(water_depth PRIVATE -Wall -Wextra)
    target_compile_options(mesh_import PRIVATE -Wall -Wextra)
    target_compile_options(mesh_diff PRIVATE -Wall -Wextra)
    target_compile_options(mesh_dump PRIVATE -Wall -Wextra)
    target_compile_options(mesh_summary PRIVATE -Wall -Wextra)
    target_compile_options(normal_analyze PRIVATE -Wall -Wextra)
    target_compile_options(normal_compare PRIVATE -Wall -Wextra)
    target_compile_options(normal_table PRIVATE -Wall -Wextra)
    target_compile_options(media_height PRIVATE -Wall -Wextra)
    target_compile_options(media_dump PRIVATE -Wall -Wextra)
    target_compile_options(core_dump PRIVATE -Wall -Wextra)
    target_compile_options(proj_dump PRIVATE -Wall -Wextra)
    target_compile_options(build_plugin PRIVATE -Wall -Wextra)
    target_compile_options(export_models PRIVATE -Wall -Wextra)
endif()

set(PACKAGE_STAGING_DIR "${CMAKE_BINARY_DIR}/package_release")
set(PACKAGE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/dist")
set(PACKAGE_COPY_COMMANDS)
foreach(target IN LISTS RELEASE_TARGETS)
    list(APPEND PACKAGE_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${target}>"
            "${PACKAGE_STAGING_DIR}/"
    )
endforeach()

add_custom_target(package_release
    DEPENDS ${RELEASE_TARGETS}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_STAGING_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_STAGING_DIR}"
    ${PACKAGE_COPY_COMMANDS}
    COMMAND ${CMAKE_COMMAND}
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DSTAGING_DIR=${PACKAGE_STAGING_DIR}"
        "-DOUTPUT_DIR=${PACKAGE_OUTPUT_DIR}"
        "-DPACKAGE_CONFIG=$<CONFIG>"
        "-DPACKAGE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DPACKAGE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
        "-DPACKAGE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
        "-DPACKAGE_HOST_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR}"
        -P "${CMAKE_SOURCE_DIR}/cmake/package_release.cmake"
    COMMENT "Packaging release executables"
    VERBATIM
)
