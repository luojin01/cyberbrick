#pragma once

#include <zephyr/kernel.h>

#define SERVO_COUNT 3

typedef enum {
    SERVO_TYPE_POSITIONAL = 0,   /* 0..180° angle */
    SERVO_TYPE_CONTINUOUS = 1,   /* -100..+100 speed (0 = stop) */
} servo_type_t;

int servo_init(void);

servo_type_t servo_get_type(uint8_t id);

/* Positional servo (SERVO_TYPE_POSITIONAL): angle 0..180. */
int servo_set_angle(uint8_t id, uint8_t angle);
uint8_t servo_get_angle(uint8_t id);

/* Continuous-rotation servo (SERVO_TYPE_CONTINUOUS): speed -100..+100, 0 = stop. */
int servo_set_speed(uint8_t id, int8_t speed);
int8_t servo_get_speed(uint8_t id);

