add_executable(rudp_test_fault
  main.cpp
  matrix.cpp
)
target_include_directories(rudp_test_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(rudp_test_fault PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_test_fault PRIVATE rudp_rt)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt fault_cases)
foreach(case_name IN LISTS fault_cases)
  if(case_name)
    add_test(NAME fault.${case_name} COMMAND rudp_test_fault --case ${case_name})
    set_tests_properties(fault.${case_name} PROPERTIES TIMEOUT 600)
  endif()
endforeach()
