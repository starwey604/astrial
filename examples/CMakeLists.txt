add_executable(01_connect 01_connect.cpp)
target_link_libraries(01_connect PRIVATE astrial)

add_executable(01_list_ports 01_list_ports.cpp)
target_link_libraries(01_list_ports PRIVATE astrial)

add_executable(02_reconnect 02_reconnect.cpp)
target_link_libraries(02_reconnect PRIVATE astrial)

add_executable(02_async_write 02_async_write.cpp)
target_link_libraries(02_async_write PRIVATE astrial)