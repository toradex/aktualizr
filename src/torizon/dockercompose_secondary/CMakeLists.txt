set(SOURCES command_runner.cc compose_manager.cc
    dockercomposesecondary.cc dockerofflineloader.cc dockertarballloader.cc)

# Recent versions of Boost (e.g. v1.87) deprecated boost::process::v1::child::wait_for since it is
# unreliable. However the new v2 API isn;t available in the 1.74 version we support in Debian
# oldstable. For now disable the warning
# https://www.boost.org/doc/libs/1_74_0/doc/html/process.html
#
set_source_files_properties(command_runner.cc PROPERTIES COMPILE_FLAGS "-Wno-deprecated-declarations")

set(HEADERS command_runner.h compose_manager.h
    dockercomposesecondary.h dockerofflineloader.h dockertarballloader.h)

set(TARGET torizon_dockercompose_secondary)

add_library(${TARGET} STATIC
  ${SOURCES}
)

target_include_directories(
  ${TARGET} PUBLIC
  ${PROJECT_SOURCE_DIR}/src/virtual_secondary
  ${PROJECT_SOURCE_DIR}/third_party/jsoncpp/include
  ${CMAKE_CURRENT_SOURCE_DIR})

add_aktualizr_test(
  NAME torizon_dockercompose_secondary
  SOURCES dockercompose_secondary_test.cc
  PROJECT_WORKING_DIRECTORY
  LIBRARIES torizon_dockercompose_secondary virtual_secondary
)

add_aktualizr_test(
    NAME command_runner
    SOURCES command_runner_test.cc
    LIBRARIES torizon_dockercompose_secondary
    NO_VALGRIND
)

set_tests_properties(test_command_runner PROPERTIES TIMEOUT 30)

aktualizr_source_file_checks(${HEADERS} ${SOURCES} ${TEST_SOURCES})

add_subdirectory("tests")
