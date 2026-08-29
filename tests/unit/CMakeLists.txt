add_executable(rudp_test_unit
  main.cpp
  wire_vectors.cpp
  core.cpp
  queues.cpp
  sender_receiver.cpp
  net_faults.cpp
)
target_include_directories(rudp_test_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(rudp_test_unit PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_test_unit PRIVATE rudp_rt)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt unit_cases)
foreach(case_name IN LISTS unit_cases)
  if(case_name)
    add_test(NAME unit.${case_name} COMMAND rudp_test_unit --case ${case_name})
    set_tests_properties(unit.${case_name} PROPERTIES TIMEOUT 120)
  endif()
endforeach()
