cmake_minimum_required(VERSION 3.10)
project(c_web_server C)

set(CMAKE_C_STANDARD 99)

add_executable(main
    main.c
    ./lib/http/http.c
    ./lib/request/request.c
    ./lib/response/response.c
)

target_link_libraries(main PRIVATE pthread)
