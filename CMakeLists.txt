cmake_minimum_required(VERSION 2.8)
include_directories(${CMAKE_CURRENT_LIST_DIR})
include_directories(/home/abakken/dev/curl/include)
# include_directories(/home/abakken/curltest/include)

# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
add_executable(curlcrash main.c)
# target_link_libraries(curlcrash /home/abakken/curltest/lib/libcurl.so)
target_link_libraries(curlcrash /home/abakken/dev/curl/lib/.libs/libcurl.so)
