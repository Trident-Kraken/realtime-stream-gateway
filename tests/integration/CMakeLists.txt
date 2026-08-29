add_executable(rudp_test_integration
  main.cpp
  scenarios.cpp
)
target_include_directories(rudp_test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(rudp_test_integration PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_test_integration PRIVATE rudp_rt)
target_compile_definitions(rudp_test_integration PRIVATE RUDP_BIN_DIR="${CMAKE_BINARY_DIR}")

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt integration_cases)
foreach(case_name IN LISTS integration_cases)
  if(case_name)
    add_test(NAME integration.${case_name} COMMAND rudp_test_integration --case ${case_name})
    set_tests_properties(integration.${case_name} PROPERTIES TIMEOUT 300)
  endif()
endforeach()
