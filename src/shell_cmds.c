/*
 * Shell commands for manual servo / DC-motor control.
 *
 *   servo set <id> <angle>     -- id 0..2, angle 0..180
 *   servo get <id>
 *   motor fwd | rev | stop
 *   motor status
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>

#include "servo.h"
#include "dc_motor.h"

/* ── servo ─────────────────────────────────────────────────────────────────── */

static int cmd_servo_set(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "usage: servo set <id 0..%d> <angle 0..180>",
                    SERVO_COUNT - 1);
        return -EINVAL;
    }

    int id = atoi(argv[1]);
    int angle = atoi(argv[2]);

    if (id < 0 || id >= SERVO_COUNT) {
        shell_error(sh, "invalid id %d (range 0..%d)", id, SERVO_COUNT - 1);
        return -EINVAL;
    }
    if (servo_get_type((uint8_t)id) != SERVO_TYPE_POSITIONAL) {
        shell_error(sh,
            "servo %d is continuous; use `servo speed %d <-100..100>`",
            id, id);
        return -ENOTSUP;
    }
    if (angle < 0 || angle > 180) {
        shell_error(sh, "invalid angle %d (range 0..180)", angle);
        return -EINVAL;
    }

    int ret = servo_set_angle((uint8_t)id, (uint8_t)angle);
    if (ret < 0) {
        shell_error(sh, "servo_set_angle failed: %d", ret);
        return ret;
    }

    shell_print(sh, "servo %d -> %d°", id, angle);
    return 0;
}

static int cmd_servo_speed(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "usage: servo speed <id 0..%d> <-100..100>",
                    SERVO_COUNT - 1);
        return -EINVAL;
    }

    int id = atoi(argv[1]);
    int speed = atoi(argv[2]);

    if (id < 0 || id >= SERVO_COUNT) {
        shell_error(sh, "invalid id %d (range 0..%d)", id, SERVO_COUNT - 1);
        return -EINVAL;
    }
    if (servo_get_type((uint8_t)id) != SERVO_TYPE_CONTINUOUS) {
        shell_error(sh,
            "servo %d is positional; use `servo set %d <0..180>`", id, id);
        return -ENOTSUP;
    }
    if (speed < -100 || speed > 100) {
        shell_error(sh, "invalid speed %d (range -100..100)", speed);
        return -EINVAL;
    }

    int ret = servo_set_speed((uint8_t)id, (int8_t)speed);
    if (ret < 0) {
        shell_error(sh, "servo_set_speed failed: %d", ret);
        return ret;
    }

    shell_print(sh, "servo %d -> speed %d", id, speed);
    return 0;
}

static void print_one(const struct shell *sh, uint8_t id)
{
    if (servo_get_type(id) == SERVO_TYPE_CONTINUOUS) {
        shell_print(sh, "servo %u: speed %d (continuous)",
                    id, servo_get_speed(id));
    } else {
        shell_print(sh, "servo %u: %u° (positional)",
                    id, servo_get_angle(id));
    }
}

static int cmd_servo_get(const struct shell *sh, size_t argc, char **argv)
{
    if (argc == 2) {
        int id = atoi(argv[1]);
        if (id < 0 || id >= SERVO_COUNT) {
            shell_error(sh, "invalid id %d", id);
            return -EINVAL;
        }
        print_one(sh, (uint8_t)id);
        return 0;
    }

    /* No id -> dump all */
    for (uint8_t i = 0; i < SERVO_COUNT; i++) {
        print_one(sh, i);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_servo,
    SHELL_CMD_ARG(set,   NULL, "Set positional servo angle: set <id> <0..180>",
                  cmd_servo_set, 3, 0),
    SHELL_CMD_ARG(speed, NULL, "Set continuous servo speed: speed <id> <-100..100>",
                  cmd_servo_speed, 3, 0),
    SHELL_CMD_ARG(get,   NULL, "Get servo state: get [id]",
                  cmd_servo_get, 1, 1),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(servo, &sub_servo, "Servo control", NULL);

/* ── motor ─────────────────────────────────────────────────────────────────── */

static const char *dir_name(motor_direction_t d)
{
    switch (d) {
    case MOTOR_FORWARD: return "forward";
    case MOTOR_REVERSE: return "reverse";
    case MOTOR_STOP:    return "stop";
    default:            return "?";
    }
}

static int set_motor(const struct shell *sh, motor_direction_t dir)
{
    int ret = dc_motor_set(dir);
    if (ret < 0) {
        shell_error(sh, "dc_motor_set failed: %d", ret);
        return ret;
    }
    shell_print(sh, "motor: %s", dir_name(dir));
    return 0;
}

static int cmd_motor_fwd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    return set_motor(sh, MOTOR_FORWARD);
}

static int cmd_motor_rev(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    return set_motor(sh, MOTOR_REVERSE);
}

static int cmd_motor_stop(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    return set_motor(sh, MOTOR_STOP);
}

static int cmd_motor_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "motor: %s", dir_name(dc_motor_get()));
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_motor,
    SHELL_CMD(fwd,    NULL, "Drive motor forward",  cmd_motor_fwd),
    SHELL_CMD(rev,    NULL, "Drive motor reverse",  cmd_motor_rev),
    SHELL_CMD(stop,   NULL, "Stop motor",           cmd_motor_stop),
    SHELL_CMD(status, NULL, "Show motor state",     cmd_motor_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(motor, &sub_motor, "DC motor control", NULL);
