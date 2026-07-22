add_executable(cws_demo demo.c)
target_link_libraries(cws_demo PRIVATE cws)
set_target_properties(cws_demo PROPERTIES
    OUTPUT_NAME cws_demo
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
