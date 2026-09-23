/*
 * Minimal assertion helpers for host unit tests. Each test binary returns
 * non-zero if any check failed, which is all CTest needs.
 */
#pragma once

#include <stdio.h>
#include <string.h>

static int g_test_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_test_failures++;                                                \
        }                                                                     \
    } while (0)

#define CHECK_STR(actual, expected)                                           \
    do {                                                                      \
        const char *a_ = (actual), *e_ = (expected);                          \
        if (strcmp(a_, e_) != 0) {                                            \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, \
                    __LINE__, e_, a_);                                        \
            g_test_failures++;                                                \
        }                                                                     \
    } while (0)

#define RUN(test)                                                             \
    do {                                                                      \
        int before_ = g_test_failures;                                        \
        test();                                                               \
        printf("%s %s\n", g_test_failures == before_ ? "PASS" : "FAIL", #test); \
    } while (0)

#define TEST_EXIT() (g_test_failures ? 1 : 0)
