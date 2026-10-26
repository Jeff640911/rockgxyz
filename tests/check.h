#ifndef MBOX_TEST_CHECK_H
#define MBOX_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int check_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            check_failures++;                                                \
        }                                                                    \
    } while (0)

#define CHECK_DONE() (check_failures ? (fprintf(stderr, "%d failure(s)\n", check_failures), 1) : 0)

#endif
