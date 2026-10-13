/* boxrig - A highly independent, general-purpose terminal emulator parser library
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/boxrig/
 */

#include "boxrig/ui/notification.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                       \
        }                                                                       \
    } while (0)

typedef struct fake_clock {
    uint64_t milliseconds;
} fake_clock;

static uint64_t read_fake_clock(void *context)
{
    fake_clock *clock = context;

    return clock->milliseconds;
}

static bool test_default_deny_and_occasions(void)
{
    static const boxrig_notification_view always = {
        true,
        {(const uint8_t *)"always", 6},
        {(const uint8_t *)"title", 5},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    static const boxrig_notification_view unfocused = {
        true,
        {(const uint8_t *)"unfocused", 9},
        {(const uint8_t *)"title", 5},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_UNFOCUSED,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    static const boxrig_notification_view invisible = {
        true,
        {(const uint8_t *)"invisible", 9},
        {(const uint8_t *)"title", 5},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_INVISIBLE,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    fake_clock clock = {100};
    boxrig_notification_policy_config config =
        boxrig_notification_policy_config_default(read_fake_clock, &clock);
    boxrig_notification_policy_state *policy = NULL;
    boxrig_notification_policy_evaluation evaluation;

    CHECK(!config.allow_notifications);
    CHECK(config.token_capacity == 5);
    CHECK(config.token_refill_milliseconds == 2000);
    CHECK(config.duplicate_milliseconds == 1000);
    CHECK(config.tracking_capacity == 64);
    CHECK(boxrig_notification_policy_create(&config, &policy) == 0);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &always,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    boxrig_notification_policy_destroy(policy);

    policy = NULL;
    config.allow_notifications = true;
    CHECK(boxrig_notification_policy_create(&config, &policy) == 0);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &unfocused,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &unfocused,
        false,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &invisible,
        false,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &invisible,
        false,
        false,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    boxrig_notification_policy_destroy(policy);
    return true;
}

static bool test_rate_limit_refill_and_clock_rollback(void)
{
    static const boxrig_notification_view notification = {
        false,
        {NULL, 0},
        {(const uint8_t *)"anonymous", 9},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    fake_clock clock = {10000};
    boxrig_notification_policy_config config =
        boxrig_notification_policy_config_default(read_fake_clock, &clock);
    boxrig_notification_policy_state *policy = NULL;
    boxrig_notification_policy_evaluation evaluation;
    size_t index;

    config.allow_notifications = true;
    CHECK(boxrig_notification_policy_create(&config, &policy) == 0);
    for (index = 0; index < 5; index++) {
        evaluation = boxrig_notification_policy_evaluate(
            policy,
            &notification,
            true,
            true,
            true);
        CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    }
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    clock.milliseconds = 11999;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    clock.milliseconds = 12000;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    clock.milliseconds = 11000;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    clock.milliseconds = 14000;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        false);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_UNAVAILABLE);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    boxrig_notification_policy_destroy(policy);
    return true;
}

static bool test_full_bucket_discards_excess_refill_time(void)
{
    static const boxrig_notification_view notification = {
        false,
        {NULL, 0},
        {(const uint8_t *)"anonymous", 9},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    fake_clock clock = {10000};
    boxrig_notification_policy_config config =
        boxrig_notification_policy_config_default(read_fake_clock, &clock);
    boxrig_notification_policy_state *policy = NULL;
    boxrig_notification_policy_evaluation evaluation;
    size_t index;

    config.allow_notifications = true;
    CHECK(boxrig_notification_policy_create(&config, &policy) == 0);
    clock.milliseconds = 20000;
    for (index = 0; index < 5; index++) {
        evaluation = boxrig_notification_policy_evaluate(
            policy,
            &notification,
            true,
            true,
            true);
        CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    }
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &notification,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DENIED);
    boxrig_notification_policy_destroy(policy);
    return true;
}

static bool test_successful_duplicates_updates_and_tracking_eviction(void)
{
    static const boxrig_notification_text_view first_types[] = {
        {(const uint8_t *)"build", 5},
        {(const uint8_t *)"status", 6},
    };
    static const boxrig_notification_view first = {
        true,
        {(const uint8_t *)"first", 5},
        {(const uint8_t *)"title", 5},
        {(const uint8_t *)"body", 4},
        {(const uint8_t *)"boxrig", 6},
        first_types,
        2,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_CRITICAL,
    };
    static const boxrig_notification_view updated = {
        true,
        {(const uint8_t *)"first", 5},
        {(const uint8_t *)"title", 5},
        {(const uint8_t *)"changed", 7},
        {(const uint8_t *)"boxrig", 6},
        first_types,
        2,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_CRITICAL,
    };
    static const boxrig_notification_view second = {
        true,
        {(const uint8_t *)"second", 6},
        {(const uint8_t *)"title", 5},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    static const boxrig_notification_view third = {
        true,
        {(const uint8_t *)"third", 5},
        {(const uint8_t *)"title", 5},
        {NULL, 0},
        {NULL, 0},
        NULL,
        0,
        BOXRIG_NOTIFICATION_ALWAYS,
        BOXRIG_NOTIFICATION_URGENCY_NORMAL,
    };
    fake_clock clock = {5000};
    boxrig_notification_policy_config config =
        boxrig_notification_policy_config_default(read_fake_clock, &clock);
    boxrig_notification_policy_state *policy = NULL;
    boxrig_notification_policy_evaluation evaluation;

    config.allow_notifications = true;
    config.tracking_capacity = 2;
    CHECK(boxrig_notification_policy_create(&config, &policy) == 0);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &first,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    CHECK(boxrig_notification_policy_record_success(
              policy,
              &first,
              evaluation.timestamp_milliseconds) == 0);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &first,
        true,
        true,
        false);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE);
    clock.milliseconds = 4990;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &first,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE);
    clock.milliseconds = 5001;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &updated,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    CHECK(boxrig_notification_policy_record_success(
              policy,
              &updated,
              evaluation.timestamp_milliseconds) == 0);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &updated,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE);

    CHECK(boxrig_notification_policy_record_success(
              policy,
              &second,
              5002) == 0);
    CHECK(boxrig_notification_policy_record_success(
              policy,
              &third,
              5003) == 0);
    clock.milliseconds = 5003;
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &updated,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DISPLAY);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &second,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE);
    evaluation = boxrig_notification_policy_evaluate(
        policy,
        &third,
        true,
        true,
        true);
    CHECK(evaluation.decision == BOXRIG_NOTIFICATION_POLICY_DUPLICATE);
    boxrig_notification_policy_destroy(policy);
    return true;
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } tests[] = {
        {"default deny and occasions", test_default_deny_and_occasions},
        {"rate limit refill and clock rollback",
         test_rate_limit_refill_and_clock_rollback},
        {"full bucket discards excess refill time",
         test_full_bucket_discards_excess_refill_time},
        {"successful duplicates updates and tracking eviction",
         test_successful_duplicates_updates_and_tracking_eviction},
    };
    size_t index;

    for (index = 0; index < sizeof(tests) / sizeof(tests[0]); index++) {
        if (!tests[index].run()) {
            fprintf(stderr, "Test failed: %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    return 0;
}
