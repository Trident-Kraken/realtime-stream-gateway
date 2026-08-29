add_executable(rudp_test_system
  main.cpp
  process.cpp
)
target_include_directories(rudp_test_system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(rudp_test_system PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_test_system PRIVATE rudp_rt)
target_compile_definitions(rudp_test_system PRIVATE RUDP_BIN_DIR="${CMAKE_BINARY_DIR}")

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt system_cases)
foreach(case_name IN LISTS system_cases)
  if(case_name)
    add_test(NAME system.${case_name} COMMAND rudp_test_system --case ${case_name})
    set_tests_properties(system.${case_name} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
  endif()
endforeach()
