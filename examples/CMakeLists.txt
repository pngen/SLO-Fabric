set(SLOFABRIC_EXAMPLES
  example_latency.cpp
  example_availability.cpp
  example_throughput.cpp
  example_recovery.cpp
  example_memory.cpp
  example_cost.cpp
  example_multobjective.cpp
  example_supersession.cpp
  example_stale_evidence.cpp
  example_enforcement_explanation.cpp
  example_error_budget.cpp)
foreach(e ${SLOFABRIC_EXAMPLES})
  string(REPLACE ".cpp" "" name ${e})
  add_executable(${name} ${e})
  target_link_libraries(${name} PRIVATE slofabric)
  slofabric_apply_warnings(${name})
endforeach()
