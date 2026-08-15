cmake_minimum_required(VERSION 3.13)
project(vwii_full_rgb)

find_package(wut REQUIRED)
find_package(wups REQUIRED)

add_wups_plugin(vwii_full_rgb
    main.c
)

target_link_libraries(vwii_full_rgb PRIVATE wut::coreinit)