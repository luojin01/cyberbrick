#include "servo.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(servo, LOG_LEVEL_INF);

/* PWM period: 20 ms = 20 000 000 ns (50 Hz) */
#define SERVO_PERIOD_NS  20000000U

/* Pulse width range in nanoseconds */
#define SERVO_MIN_NS  (CONFIG_CYBERBRICK_SERVO_MIN_US * 1000U)
#define SERVO_MAX_NS  (CONFIG_CYBERBRICK_SERVO_MAX_US * 1000U)
#define SERVO_MID_NS  ((SERVO_MIN_NS + SERVO_MAX_NS) / 2U)

static const struct pwm_dt_spec servos[SERVO_COUNT] = {
    PWM_DT_SPEC_GET(DT_ALIAS(servo0)),
    PWM_DT_SPEC_GET(DT_ALIAS(servo1)),
    PWM_DT_SPEC_GET(DT_ALIAS(servo2)),
};

/*
 * Per-servo type:
 *   id 0 ("Servo 1") = 360° continuous rotation
 *   id 1 ("Servo 2") = 360° continuous rotation
 *   id 2 ("Servo 3") = 180° positional
 */
static const servo_type_t servo_types[SERVO_COUNT] = {
    SERVO_TYPE_CONTINUOUS,
    SERVO_TYPE_CONTINUOUS,
    SERVO_TYPE_POSITIONAL,
};

/* Cached last-set values (union by index — angle for positional, speed for continuous) */
static uint8_t angles[SERVO_COUNT];
static int8_t  speeds[SERVO_COUNT];

servo_type_t servo_get_type(uint8_t id)
{
    if (id >= SERVO_COUNT) {
        return SERVO_TYPE_POSITIONAL;
    }
    return servo_types[id];
}

static int pwm_apply(uint8_t id, uint32_t pulse_ns)
{
    int ret = pwm_set_dt(&servos[id], SERVO_PERIOD_NS, pulse_ns);
    if (ret < 0) {
        LOG_ERR("Servo %d set failed: %d", id, ret);
    }
    return ret;
}

int servo_init(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (!pwm_is_ready_dt(&servos[i])) {
            LOG_ERR("Servo %d PWM device not ready", i);
            return -ENODEV;
        }
    }

    /* Safe defaults: continuous → stop, positional → center */
    for (uint8_t i = 0; i < SERVO_COUNT; i++) {
        int ret;
        if (servo_types[i] == SERVO_TYPE_CONTINUOUS) {
            ret = servo_set_speed(i, 0);
        } else {
            ret = servo_set_angle(i, 90);
        }
        if (ret < 0) {
            return ret;
        }
    }

    LOG_INF("Servo subsystem ready");
    return 0;
}

int servo_set_angle(uint8_t id, uint8_t angle)
{
    if (id >= SERVO_COUNT) {
        return -EINVAL;
    }
    if (servo_types[id] != SERVO_TYPE_POSITIONAL) {
        LOG_WRN("Servo %d is continuous; use servo_set_speed()", id);
        return -ENOTSUP;
    }
    if (angle > 180) {
        angle = 180;
    }

    uint32_t pulse_ns = SERVO_MIN_NS +
        ((uint32_t)angle * (SERVO_MAX_NS - SERVO_MIN_NS)) / 180U;

    int ret = pwm_apply(id, pulse_ns);
    if (ret < 0) {
        return ret;
    }

    angles[id] = angle;
    LOG_DBG("Servo %d → %d° (pulse %u ns)", id, angle, pulse_ns);
    return 0;
}

uint8_t servo_get_angle(uint8_t id)
{
    if (id >= SERVO_COUNT) {
        return 0;
    }
    return angles[id];
}

int servo_set_speed(uint8_t id, int8_t speed)
{
    if (id >= SERVO_COUNT) {
        return -EINVAL;
    }
    if (servo_types[id] != SERVO_TYPE_CONTINUOUS) {
        LOG_WRN("Servo %d is positional; use servo_set_angle()", id);
        return -ENOTSUP;
    }
    if (speed >  100) speed =  100;
    if (speed < -100) speed = -100;

    /*
     * speed:  -100  ->  SERVO_MIN_NS   (full one direction)
     *            0  ->  SERVO_MID_NS   (stop)
     *         +100  ->  SERVO_MAX_NS   (full other direction)
     */
    int32_t half_span = (int32_t)((SERVO_MAX_NS - SERVO_MIN_NS) / 2U);
    int32_t pulse_ns  = (int32_t)SERVO_MID_NS + (speed * half_span) / 100;

    int ret = pwm_apply(id, (uint32_t)pulse_ns);
    if (ret < 0) {
        return ret;
    }

    speeds[id] = speed;
    LOG_DBG("Servo %d → speed %d (pulse %d ns)", id, speed, pulse_ns);
    return 0;
}

int8_t servo_get_speed(uint8_t id)
{
    if (id >= SERVO_COUNT) {
        return 0;
    }
    return speeds[id];
}

