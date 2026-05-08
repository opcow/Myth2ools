if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()
if(NOT DEFINED STAGING_DIR)
    message(FATAL_ERROR "STAGING_DIR is required")
endif()
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "OUTPUT_DIR is required")
endif()
if(NOT DEFINED GIT_EXECUTABLE)
    message(FATAL_ERROR "GIT_EXECUTABLE is required")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_TAG
    ERROR_VARIABLE GIT_TAG_ERROR
    RESULT_VARIABLE GIT_TAG_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT GIT_TAG_RESULT EQUAL 0 OR GIT_TAG STREQUAL "")
    message(FATAL_ERROR "Failed to determine latest git tag: ${GIT_TAG_ERROR}")
endif()

string(REGEX REPLACE "[^A-Za-z0-9._-]" "_" SAFE_GIT_TAG "${GIT_TAG}")
set(ARCHIVE_PATH "${OUTPUT_DIR}/myth2ools_${SAFE_GIT_TAG}.zip")

file(GLOB PACKAGE_FILES RELATIVE "${STAGING_DIR}" "${STAGING_DIR}/*")
if(PACKAGE_FILES STREQUAL "")
    message(FATAL_ERROR "No files found to package in ${STAGING_DIR}")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(REMOVE "${ARCHIVE_PATH}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE_PATH}" --format=zip ${PACKAGE_FILES}
    WORKING_DIRECTORY "${STAGING_DIR}"
    RESULT_VARIABLE ZIP_RESULT
)

if(NOT ZIP_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to create ${ARCHIVE_PATH}")
endif()

message(STATUS "Created ${ARCHIVE_PATH}")
