#pragma once

#include <zephyr/kernel.h>

typedef enum {
    MOTOR_STOP = 0,
    MOTOR_FORWARD,
    MOTOR_REVERSE,
} motor_direction_t;

int dc_motor_init(void);
int dc_motor_set(motor_direction_t dir);
motor_direction_t dc_motor_get(void);
