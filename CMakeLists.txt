cmake_minimum_required(VERSION 3.10)
project(myth2ools)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Git REQUIRED)
include(FetchContent)

option(MYTH2OOLS_ENABLE_WAYLAND "Enable Wayland support in GLFW on Linux" OFF)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    if(MYTH2OOLS_ENABLE_WAYLAND)
        set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    else()
        set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    endif()
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.2b-docking
)
FetchContent_MakeAvailable(imgui)

find_package(OpenGL REQUIRED)

add_library(imgui_lib
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
)
target_include_directories(imgui_lib
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

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
add_executable(export_map_actions myth2/export_map_actions.cpp)
add_executable(export_map_objects myth2/export_map_objects.cpp)
add_executable(build_mesh myth2/build_mesh.cpp)
add_executable(mesh_verify myth2/mesh_verify.cpp)
add_executable(myth2ools_gui gui/myth2ools_gui.cpp)
target_link_libraries(myth2ools_gui PRIVATE imgui_lib)
if(WIN32)
    target_link_libraries(myth2ools_gui PRIVATE Ole32 Shell32)
endif()

set(GUI_TARGET myth2ools_gui)
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
    export_map_actions
    export_map_objects
    build_mesh
    myth2ools_gui
)
set(CLI_RELEASE_TARGETS ${RELEASE_TARGETS})
list(REMOVE_ITEM CLI_RELEASE_TARGETS ${GUI_TARGET})

set(M2OOLS_GUI_DIR "${CMAKE_SOURCE_DIR}/myth2ools_gui")
set(M2OOLS_GUI_BIN_DIR "${M2OOLS_GUI_DIR}/bin")
set(M2OOLS_GUI_SCRIPTS_DIR "${M2OOLS_GUI_DIR}/scripts")
set(M2OOLS_GUI_TOOLS_DIR "${M2OOLS_GUI_DIR}/tools")

if(EXISTS "${CMAKE_SOURCE_DIR}/scripts")
    set(M2OOLS_GUI_SOURCE_SCRIPTS_DIR "${CMAKE_SOURCE_DIR}/scripts")
else()
    set(M2OOLS_GUI_SOURCE_SCRIPTS_DIR "${M2OOLS_GUI_SCRIPTS_DIR}")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/tools")
    set(M2OOLS_GUI_SOURCE_TOOLS_DIR "${CMAKE_SOURCE_DIR}/tools")
else()
    set(M2OOLS_GUI_SOURCE_TOOLS_DIR "${M2OOLS_GUI_TOOLS_DIR}")
endif()

set(CLI_EXECUTABLE_TARGETS
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
    export_map_actions
    export_map_objects
    build_mesh
    mesh_verify
)

foreach(target IN LISTS CLI_EXECUTABLE_TARGETS)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${M2OOLS_GUI_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${M2OOLS_GUI_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${M2OOLS_GUI_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${M2OOLS_GUI_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${M2OOLS_GUI_BIN_DIR}"
    )
endforeach()

set_target_properties(${GUI_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${M2OOLS_GUI_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${M2OOLS_GUI_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${M2OOLS_GUI_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${M2OOLS_GUI_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${M2OOLS_GUI_DIR}"
)

file(GLOB M2OOLS_GUI_TOOL_FILES "${M2OOLS_GUI_SOURCE_TOOLS_DIR}/*.py")
file(GLOB M2OOLS_GUI_SCRIPT_FILES "${M2OOLS_GUI_SOURCE_SCRIPTS_DIR}/*")

set(M2OOLS_GUI_ASSET_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory "${M2OOLS_GUI_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${M2OOLS_GUI_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${M2OOLS_GUI_SCRIPTS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${M2OOLS_GUI_TOOLS_DIR}"
)

if(NOT M2OOLS_GUI_SOURCE_SCRIPTS_DIR STREQUAL M2OOLS_GUI_SCRIPTS_DIR)
    foreach(script_file IN LISTS M2OOLS_GUI_SCRIPT_FILES)
        if(NOT IS_DIRECTORY "${script_file}")
            list(APPEND M2OOLS_GUI_ASSET_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${script_file}"
                    "${M2OOLS_GUI_SCRIPTS_DIR}/"
            )
        endif()
    endforeach()
endif()

if(NOT M2OOLS_GUI_SOURCE_TOOLS_DIR STREQUAL M2OOLS_GUI_TOOLS_DIR)
    foreach(tool_file IN LISTS M2OOLS_GUI_TOOL_FILES)
        list(APPEND M2OOLS_GUI_ASSET_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${tool_file}"
                "${M2OOLS_GUI_TOOLS_DIR}/"
        )
    endforeach()
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/blender_path.txt")
    list(APPEND M2OOLS_GUI_ASSET_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/blender_path.txt"
            "${M2OOLS_GUI_DIR}/"
    )
endif()

add_custom_target(myth2ools_gui_assets ALL
    ${M2OOLS_GUI_ASSET_COMMANDS}
    COMMENT "Syncing Myth2ools GUI layout"
    VERBATIM
)

add_dependencies(${GUI_TARGET} myth2ools_gui_assets)

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
    target_compile_options(export_map_actions PRIVATE /W3)
    target_compile_options(export_map_objects PRIVATE /W3)
    target_compile_options(build_mesh PRIVATE /W3)
    target_compile_options(mesh_verify PRIVATE /W3)
    target_compile_options(myth2ools_gui PRIVATE /W3)
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
    target_compile_options(export_map_actions PRIVATE -Wall -Wextra)
    target_compile_options(export_map_objects PRIVATE -Wall -Wextra)
    target_compile_options(build_mesh PRIVATE -Wall -Wextra)
    target_compile_options(mesh_verify PRIVATE -Wall -Wextra)
    target_compile_options(myth2ools_gui PRIVATE -Wall -Wextra)
endif()

set(PACKAGE_STAGING_DIR "${CMAKE_BINARY_DIR}/package_release")
set(PACKAGE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/dist")
set(PACKAGE_COPY_COMMANDS)
list(APPEND PACKAGE_COPY_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PACKAGE_STAGING_DIR}/myth2ools_gui/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PACKAGE_STAGING_DIR}/myth2ools_gui/scripts"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${PACKAGE_STAGING_DIR}/myth2ools_gui/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${GUI_TARGET}>"
        "${PACKAGE_STAGING_DIR}/myth2ools_gui/"
)
foreach(target IN LISTS CLI_RELEASE_TARGETS)
    list(APPEND PACKAGE_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${target}>"
            "${PACKAGE_STAGING_DIR}/myth2ools_gui/bin/"
    )
endforeach()
foreach(script_file IN LISTS M2OOLS_GUI_SCRIPT_FILES)
    if(NOT IS_DIRECTORY "${script_file}")
        list(APPEND PACKAGE_COPY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${script_file}"
                "${PACKAGE_STAGING_DIR}/myth2ools_gui/scripts/"
        )
    endif()
endforeach()

foreach(tool_file IN LISTS M2OOLS_GUI_TOOL_FILES)
    if(NOT IS_DIRECTORY "${tool_file}")
        list(APPEND PACKAGE_COPY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${tool_file}"
                "${PACKAGE_STAGING_DIR}/myth2ools_gui/tools/"
        )
    endif()
endforeach()

if(EXISTS "${M2OOLS_GUI_DIR}/imgui.ini")
    list(APPEND PACKAGE_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${M2OOLS_GUI_DIR}/imgui.ini"
            "${PACKAGE_STAGING_DIR}/myth2ools_gui/"
    )
endif()

if(EXISTS "${M2OOLS_GUI_DIR}/blender_path.txt")
    list(APPEND PACKAGE_COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${M2OOLS_GUI_DIR}/blender_path.txt"
            "${PACKAGE_STAGING_DIR}/myth2ools_gui/"
    )
endif()

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
