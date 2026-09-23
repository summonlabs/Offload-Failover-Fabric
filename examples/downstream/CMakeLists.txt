cmake_minimum_required(VERSION 3.20)
project(OffloadFailoverFabricDownstream LANGUAGES CXX)

# The consumer uses only the installed package: no source paths, no exported
# warning flags, no build-tree includes.
find_package(OffloadFailoverFabric 1.0 REQUIRED CONFIG)

add_executable(off_downstream main.cpp)
target_link_libraries(off_downstream PRIVATE OffloadFailoverFabric::offload_failover_fabric)
set_target_properties(off_downstream PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
