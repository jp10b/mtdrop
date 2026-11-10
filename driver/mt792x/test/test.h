/*
 * test.h - minimal single-header unit test framework for C11.
 *
 * Usage:
 *   - In every test file:   #include "test.h"
 *                           TEST(my_test) { CHECK(1 + 1 == 2); }
 *     TEST() registers itself before main() runs (constructor attribute on
 *     GCC/Clang, .CRT$XCU section on MSVC).
 *   - In exactly ONE translation unit: #define MT_TEST_MAIN before including
 *     this header. That unit provides the runner and main().
 *
 * Checks do not abort the test; each failure prints file:line and the test
 * continues. The process exit code is non-zero if any check failed.
 * Optional argv[1]: run only tests whose name contains that substring.
 */
#ifndef MT_TEST_H
#define MT_TEST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*mt_test_fn)(void);

void mt_test_register(const char *name, mt_test_fn fn);
void mt_test_fail_check(const char *file, int line, const char *expr);
void mt_test_fail_eq(const char *file, int line, const char *ea, const char *eb,
                     uint32_t a, uint32_t b);
void mt_test_fail_mem(const char *file, int line, const char *ea, const char *eb,
                      size_t len, size_t first_diff);

#if defined(_MSC_VER)
#define MT_TEST_REGISTER_(name)                                                  \
    static void mt_test_reg_##name(void);                                        \
    __pragma(section(".CRT$XCU", read))                                          \
    __declspec(allocate(".CRT$XCU")) static void (*mt_test_ptr_##name)(void) =   \
        mt_test_reg_##name;                                                      \
    static void mt_test_reg_##name(void) { mt_test_register(#name, mt_test_##name); }
#else
#define MT_TEST_REGISTER_(name)                                                  \
    __attribute__((constructor)) static void mt_test_reg_##name(void)            \
    {                                                                            \
        mt_test_register(#name, mt_test_##name);                                 \
    }
#endif

#define TEST(name)                                                               \
    static void mt_test_##name(void);                                            \
    MT_TEST_REGISTER_(name)                                                      \
    static void mt_test_##name(void)

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) mt_test_fail_check(__FILE__, __LINE__, #cond);              \
    } while (0)

#define CHECK_EQ_U32(a, b)                                                       \
    do {                                                                         \
        uint32_t mt_a_ = (uint32_t)(a);                                          \
        uint32_t mt_b_ = (uint32_t)(b);                                          \
        if (mt_a_ != mt_b_) mt_test_fail_eq(__FILE__, __LINE__, #a, #b, mt_a_, mt_b_); \
    } while (0)

#define CHECK_MEM_EQ(a, b, len)                                                  \
    do {                                                                         \
        const unsigned char *mt_pa_ = (const unsigned char *)(a);                \
        const unsigned char *mt_pb_ = (const unsigned char *)(b);                \
        size_t mt_len_ = (size_t)(len);                                          \
        size_t mt_i_ = 0;                                                        \
        while (mt_i_ < mt_len_ && mt_pa_[mt_i_] == mt_pb_[mt_i_]) mt_i_++;       \
        if (mt_i_ != mt_len_) mt_test_fail_mem(__FILE__, __LINE__, #a, #b, mt_len_, mt_i_); \
    } while (0)

#endif /* MT_TEST_H */

/* ---- runner (one translation unit only) ---- */
#if defined(MT_TEST_MAIN) && !defined(MT_TEST_MAIN_DONE_)
#define MT_TEST_MAIN_DONE_

#include <stdio.h>

#define MT_TEST_MAX 1024

static struct {
    const char *name;
    mt_test_fn fn;
} mt_tests_[MT_TEST_MAX];
static size_t mt_test_count_;
static unsigned mt_cur_failed_;     /* failed checks in the running test */
static unsigned mt_total_failed_;   /* failed checks overall */
static const char *mt_cur_name_ = "";

void mt_test_register(const char *name, mt_test_fn fn)
{
    if (mt_test_count_ < MT_TEST_MAX) {
        mt_tests_[mt_test_count_].name = name;
        mt_tests_[mt_test_count_].fn = fn;
        mt_test_count_++;
    }
}

void mt_test_fail_check(const char *file, int line, const char *expr)
{
    printf("  %s:%d: CHECK failed: %s\n", file, line, expr);
    mt_cur_failed_++;
}

void mt_test_fail_eq(const char *file, int line, const char *ea, const char *eb,
                     uint32_t a, uint32_t b)
{
    printf("  %s:%d: CHECK_EQ_U32 failed: %s == %s\n    left  = 0x%08lX\n    right = 0x%08lX\n",
           file, line, ea, eb, (unsigned long)a, (unsigned long)b);
    mt_cur_failed_++;
}

void mt_test_fail_mem(const char *file, int line, const char *ea, const char *eb,
                      size_t len, size_t first_diff)
{
    printf("  %s:%d: CHECK_MEM_EQ failed: %s vs %s (%lu bytes), first difference at byte %lu\n",
           file, line, ea, eb, (unsigned long)len, (unsigned long)first_diff);
    mt_cur_failed_++;
}

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;
    size_t i;
    size_t ran = 0, failed_tests = 0;

    for (i = 0; i < mt_test_count_; i++) {
        if (filter && strstr(mt_tests_[i].name, filter) == NULL) continue;
        mt_cur_name_ = mt_tests_[i].name;
        mt_cur_failed_ = 0;
        printf("[ RUN  ] %s\n", mt_cur_name_);
        mt_tests_[i].fn();
        ran++;
        if (mt_cur_failed_) {
            failed_tests++;
            mt_total_failed_ += mt_cur_failed_;
            printf("[ FAIL ] %s (%u failed checks)\n", mt_cur_name_, mt_cur_failed_);
        } else {
            printf("[  OK  ] %s\n", mt_cur_name_);
        }
    }
    printf("\n%lu test(s) run, %lu passed, %lu failed (%u failed checks)\n",
           (unsigned long)ran, (unsigned long)(ran - failed_tests),
           (unsigned long)failed_tests, mt_total_failed_);
    if (ran == 0) {
        printf("no tests matched\n");
        return 1;
    }
    return failed_tests ? 1 : 0;
}

#endif /* MT_TEST_MAIN */
