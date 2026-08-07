/* ami2ha -- minimal test harness (host builds only) */
#ifndef TINYTEST_H
#define TINYTEST_H

#include <stdio.h>
#include <string.h>

extern int tt_checks;
extern int tt_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        tt_checks++;                                                       \
        if (!(cond)) {                                                     \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            tt_failures++;                                                 \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want)                                               \
    do {                                                                   \
        const char *g_ = (got), *w_ = (want);                              \
        tt_checks++;                                                       \
        if (strcmp(g_, w_) != 0) {                                         \
            printf("    FAIL %s:%d: got \"%s\", want \"%s\"\n",            \
                   __FILE__, __LINE__, g_, w_);                            \
            tt_failures++;                                                 \
        }                                                                  \
    } while (0)

#define CHECK_INT(got, want)                                               \
    do {                                                                   \
        long g_ = (long)(got), w_ = (long)(want);                          \
        tt_checks++;                                                       \
        if (g_ != w_) {                                                    \
            printf("    FAIL %s:%d: %s = %ld, want %ld\n",                 \
                   __FILE__, __LINE__, #got, g_, w_);                      \
            tt_failures++;                                                 \
        }                                                                  \
    } while (0)

#define RUN(fn)                                                            \
    do {                                                                   \
        printf("  %s\n", #fn);                                             \
        fn();                                                              \
    } while (0)

#endif /* TINYTEST_H */
