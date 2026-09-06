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

if(CMAKE_TESTING_ENABLED)
  add_test(NAME slofabric_distributed_proof
    COMMAND $<TARGET_FILE:slofabric_distributed_proof>
            $<TARGET_FILE:slofabric_coordinator>
            $<TARGET_FILE:slofabric_worker>
            45701)
  set_tests_properties(slofabric_distributed_proof PROPERTIES
    WORKING_DIRECTORY $<TARGET_FILE_DIR:slofabric_distributed_proof>)
endif()
