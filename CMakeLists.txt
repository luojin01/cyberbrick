cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(cyberbrick_lone VERSION 1.0.0)

target_sources(app PRIVATE
    src/main.c
    src/servo.c
    src/dc_motor.c
    src/web_server.c
    src/shell_cmds.c
)
