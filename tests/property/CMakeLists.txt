add_executable(rudp_test_property
  main.cpp
  properties.cpp
)
target_include_directories(rudp_test_property PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(rudp_test_property PRIVATE ${RUDP_WARNING_FLAGS})
target_link_libraries(rudp_test_property PRIVATE rudp_rt)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/cases.txt property_cases)
foreach(case_name IN LISTS property_cases)
  if(case_name)
    add_test(NAME property.${case_name} COMMAND rudp_test_property --case ${case_name})
    set_tests_properties(property.${case_name} PROPERTIES TIMEOUT 300)
  endif()
endforeach()
