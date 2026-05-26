#include "dc_motor.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(dc_motor, LOG_LEVEL_INF);

static const struct gpio_dt_spec in1 =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), motor_in1_gpios);
static const struct gpio_dt_spec in2 =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), motor_in2_gpios);

static motor_direction_t current_dir = MOTOR_STOP;

int dc_motor_init(void)
{
    if (!gpio_is_ready_dt(&in1) || !gpio_is_ready_dt(&in2)) {
        LOG_ERR("DC motor GPIO not ready");
        return -ENODEV;
    }

    int ret;

    ret = gpio_pin_configure_dt(&in1, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&in2, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    LOG_INF("DC motor ready");
    return 0;
}

int dc_motor_set(motor_direction_t dir)
{
    int ret;

    switch (dir) {
    case MOTOR_FORWARD:
        /* Forward: IN1=HIGH, IN2=LOW */
        ret = gpio_pin_set_dt(&in1, 1);
        if (ret < 0) return ret;
        ret = gpio_pin_set_dt(&in2, 0);
        break;
    case MOTOR_REVERSE:
        ret = gpio_pin_set_dt(&in1, 0);
        if (ret < 0) return ret;
        ret = gpio_pin_set_dt(&in2, 1);
        break;
    case MOTOR_STOP:
    default:
        ret = gpio_pin_set_dt(&in1, 0);
        if (ret < 0) return ret;
        ret = gpio_pin_set_dt(&in2, 0);
        break;
    }

    if (ret < 0) {
        LOG_ERR("Motor GPIO set failed: %d", ret);
        return ret;
    }

    current_dir = dir;
    LOG_DBG("Motor → %s", dir == MOTOR_FORWARD ? "forward" :
                           dir == MOTOR_REVERSE ? "reverse" : "stop");
    return 0;
}

motor_direction_t dc_motor_get(void)
{
    return current_dir;
}
