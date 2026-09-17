include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cps)

# The relax-experiments harness addresses binaries as <name>-<weight type>,
# matching the weight type the graph file itself was written with.
add_executable(sssp-cpu-int32 SSSP.cpp)
add_dependencies(apps sssp-cpu-int32)
target_link_libraries(sssp-cpu-int32 PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-cpu-int32 DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_executable(sssp-cpu-float SSSP.cpp)
add_dependencies(apps sssp-cpu-float)
target_compile_definitions(sssp-cpu-float PRIVATE USE_FLOAT)
target_link_libraries(sssp-cpu-float PRIVATE Galois::shmem lonestar)
install(TARGETS sssp-cpu-float DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_custom_target(sssp-cpu DEPENDS sssp-cpu-int32 sssp-cpu-float)

add_test_scale(small1 sssp-cpu-int32 "${BASEINPUT}/reference/structured/rome99.gr" -delta 8)
add_test_scale(small2 sssp-cpu-int32 "${BASEINPUT}/scalefree/rmat10.gr" -delta 8)
