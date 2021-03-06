// Copyright 2016 The Fuchsia Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <unistd.h>

#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <unittest/unittest.h>
#include <runtime/thread.h>


static const char kThreadName[] = "test-thread";

static void test_thread_fn(void* arg) {
    // Note: You shouldn't use C standard library functions from this thread.
    mx_nanosleep(MX_MSEC(100));
    mx_thread_exit();
}

static void busy_thread_fn(void* arg) {
    volatile uint64_t i = 0;
    while (true) {
        ++i;
    }
    __builtin_trap();
}

static void sleep_thread_fn(void* arg) {
    mx_nanosleep(MX_TIME_INFINITE);
    __builtin_trap();
}

static void wait_thread_fn(void* arg) {
    mx_handle_t event = *(mx_handle_t*)arg;
    mx_object_wait_one(event, MX_USER_SIGNAL_0, MX_TIME_INFINITE, NULL);
    __builtin_trap();
}

static bool start_thread(mxr_thread_entry_t entry, void* arg,
                         mxr_thread_t* thread_out) {
    const size_t stack_size = 256u << 10;
    mx_handle_t thread_stack_vmo;
    ASSERT_EQ(mx_vmo_create(stack_size, 0, &thread_stack_vmo), NO_ERROR, "");
    ASSERT_GT(thread_stack_vmo, 0, "");

    uintptr_t stack = 0u;
    ASSERT_EQ(mx_vmar_map(mx_vmar_root_self(), 0, thread_stack_vmo, 0, stack_size,
                          MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE, &stack), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(thread_stack_vmo), NO_ERROR, "");

    ASSERT_EQ(mxr_thread_create(mx_process_self(), "test_thread", false,
                                thread_out),
              NO_ERROR, "");
    ASSERT_EQ(mxr_thread_start(thread_out, stack, stack_size, entry, arg),
              NO_ERROR, "");
    return true;
}

static bool start_and_kill_thread(mxr_thread_entry_t entry, void* arg) {
    mxr_thread_t thread;
    ASSERT_TRUE(start_thread(entry, arg, &thread), "");
    mx_nanosleep(MX_MSEC(100));
    ASSERT_EQ(mxr_thread_kill(&thread), NO_ERROR, "");
    ASSERT_EQ(mxr_thread_join(&thread), NO_ERROR, "");
    return true;
}

static bool test_basics(void) {
    BEGIN_TEST;
    mxr_thread_t thread;
    ASSERT_TRUE(start_thread(test_thread_fn, NULL, &thread), "");
    ASSERT_EQ(mx_object_wait_one(mxr_thread_get_handle(&thread),
                                 MX_THREAD_SIGNALED, MX_TIME_INFINITE, NULL),
              NO_ERROR, "");
    mxr_thread_destroy(&thread);
    END_TEST;
}

static bool test_long_name_succeeds(void) {
    BEGIN_TEST;
    // Creating a thread with a super long name should succeed.
    static const char long_name[] =
        "0123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789";
    ASSERT_GT(strlen(long_name), (size_t)MX_MAX_NAME_LEN-1,
              "too short to truncate");

    mxr_thread_t thread;
    ASSERT_EQ(mxr_thread_create(mx_process_self(), long_name, false, &thread),
              NO_ERROR, "");
    mxr_thread_destroy(&thread);
    END_TEST;
}

// mx_thread_start() is not supposed to be usable for creating a
// process's first thread.  That's what mx_process_start() is for.
// Check that mx_thread_start() returns an error in this case.
static bool test_thread_start_on_initial_thread(void) {
    BEGIN_TEST;

    static const char kProcessName[] = "test-proc-thread1";
    mx_handle_t process;
    mx_handle_t vmar;
    mx_handle_t thread;
    ASSERT_EQ(mx_process_create(mx_job_default(), kProcessName, sizeof(kProcessName) - 1,
                                0, &process, &vmar), NO_ERROR, "");
    ASSERT_EQ(mx_thread_create(process, kThreadName, sizeof(kThreadName) - 1,
                               0, &thread), NO_ERROR, "");
    ASSERT_EQ(mx_thread_start(thread, 1, 1, 1, 1), ERR_BAD_STATE, "");

    ASSERT_EQ(mx_handle_close(thread), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(vmar), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(process), NO_ERROR, "");

    END_TEST;
}

// Test that we don't get an assertion failure (and kernel panic) if we
// pass a zero instruction pointer when starting a thread (in this case via
// mx_process_start()).
static bool test_thread_start_with_zero_instruction_pointer(void) {
    BEGIN_TEST;

    static const char kProcessName[] = "test-proc-thread2";
    mx_handle_t process;
    mx_handle_t vmar;
    mx_handle_t thread;
    ASSERT_EQ(mx_process_create(mx_job_default(), kProcessName, sizeof(kProcessName) - 1,
                                0, &process, &vmar), NO_ERROR, "");
    ASSERT_EQ(mx_thread_create(process, kThreadName, sizeof(kThreadName) - 1,
                               0, &thread), NO_ERROR, "");
    ASSERT_EQ(mx_process_start(process, thread, 0, 0, thread, 0), NO_ERROR, "");

    // Give crashlogger a little time to print info about the new thread
    // (since it will start and crash), otherwise that output gets
    // interleaved with the test runner's output.
    mx_nanosleep(MX_MSEC(100));

    ASSERT_EQ(mx_handle_close(process), NO_ERROR, "");
    ASSERT_EQ(mx_handle_close(vmar), NO_ERROR, "");

    END_TEST;
}

static bool test_kill_busy_thread(void) {
    BEGIN_TEST;

    ASSERT_TRUE(start_and_kill_thread(busy_thread_fn, NULL), "");

    END_TEST;
}

static bool test_kill_sleep_thread(void) {
    BEGIN_TEST;

    ASSERT_TRUE(start_and_kill_thread(sleep_thread_fn, NULL), "");

    END_TEST;
}

static bool test_kill_wait_thread(void) {
    BEGIN_TEST;

    mx_handle_t event;
    ASSERT_EQ(mx_event_create(0, &event), NO_ERROR, "");
    ASSERT_TRUE(start_and_kill_thread(wait_thread_fn, &event), "");
    ASSERT_EQ(mx_handle_close(event), NO_ERROR, "");

    END_TEST;
}

static bool test_info_task_stats_fails(void) {
    BEGIN_TEST;
    // Spin up a thread.
    mxr_thread_t thread;
    ASSERT_TRUE(start_thread(test_thread_fn, NULL, &thread), "");
    mx_handle_t thandle = mxr_thread_get_handle(&thread);
    ASSERT_EQ(mx_object_wait_one(thandle,
                                 MX_THREAD_SIGNALED, MX_TIME_INFINITE, NULL),
              NO_ERROR, "");

    // Ensure that task_stats doesn't work on it.
    mx_info_task_stats_t info;
    EXPECT_NEQ(mx_object_get_info(thandle, MX_INFO_TASK_STATS,
                                  &info, sizeof(info), NULL, NULL),
               NO_ERROR,
               "Just added thread support to info_task_status?");
    // If so, replace this with a real test; see example in process.cpp.

    // Clean up the thread.
    mxr_thread_destroy(&thread);
    END_TEST;
}

BEGIN_TEST_CASE(threads_tests)
RUN_TEST(test_basics)
RUN_TEST(test_long_name_succeeds)
RUN_TEST(test_thread_start_on_initial_thread)
RUN_TEST(test_thread_start_with_zero_instruction_pointer)
RUN_TEST(test_kill_busy_thread)
RUN_TEST(test_kill_sleep_thread)
RUN_TEST(test_kill_wait_thread)
RUN_TEST(test_info_task_stats_fails)
END_TEST_CASE(threads_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif
