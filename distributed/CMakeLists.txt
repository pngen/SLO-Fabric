set(SLOFABRIC_DIST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(slofabric_coordinator coordinator_main.cpp)
target_link_libraries(slofabric_coordinator PRIVATE slofabric)
target_include_directories(slofabric_coordinator PRIVATE ${SLOFABRIC_DIST_INCLUDES})
slofabric_apply_warnings(slofabric_coordinator)

add_executable(slofabric_worker worker_main.cpp)
target_link_libraries(slofabric_worker PRIVATE slofabric)
target_include_directories(slofabric_worker PRIVATE ${SLOFABRIC_DIST_INCLUDES})
slofabric_apply_warnings(slofabric_worker)

add_executable(slofabric_distributed_proof distributed_proof.cpp)
target_link_libraries(slofabric_distributed_proof PRIVATE slofabric)
target_include_directories(slofabric_distributed_proof PRIVATE ${SLOFABRIC_DIST_INCLUDES})
slofabric_apply_warnings(slofabric_distributed_proof)

if(BUILD_TESTING)
  add_test(NAME slofabric_distributed_proof
    COMMAND slofabric_distributed_proof
      ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/slofabric_coordinator.exe
      ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/slofabric_worker.exe
      45701)
  set_tests_properties(slofabric_distributed_proof PROPERTIES
    ENVIRONMENT "PATH=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR};$ENV{PATH}")
endif()
