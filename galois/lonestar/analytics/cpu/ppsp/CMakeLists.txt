add_executable(ppsp-cpu ppsp.cpp)
add_dependencies(apps ppsp-cpu)
target_link_libraries(ppsp-cpu PRIVATE Galois::shmem lonestar)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cps)
install(TARGETS ppsp-cpu DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT apps EXCLUDE_FROM_ALL)

add_test_scale(small1 ppsp-cpu "${BASEINPUT}/reference/structured/rome99.gr" -delta 8)
add_test_scale(small2 ppsp-cpu "${BASEINPUT}/scalefree/rmat10.gr" -delta 8)
