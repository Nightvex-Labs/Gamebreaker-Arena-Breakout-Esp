// ============================================================================
// TEST_TRACE — heavy instrumentation for tomorrow's local crash-hunt.
// Every call site tagged for easy grep+delete after diagnosis:
//   grep -rn "TEST_TRACE\|TEST-REMOVE" src/ inc/
//   → delete every hit before the next real release.
//
// Wraps ah_diag() (reader-side, C linkage) and LOG() (overlay-side, C++).
// Both funnel through the same telemetry pipeline uploaded to koenflow.com.
// ============================================================================
#pragma once

// TEST-REMOVE: whole file — delete after crash-hunt
#ifdef __cplusplus
extern "C" {
#endif

// ah_diag is declared in ah_reader_thread.cpp translation unit only. For
// non-reader callers we forward to a helper that writes the same log file.
// Declared here so any TU can #include this header and start tracing.
void ah_test_trace_write(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

// Single entry point. Grep for TEST_TRACE to find every touch.
#define TEST_TRACE(fmt, ...)  ah_test_trace_write("[TT %s:%d] " fmt, \
                                                   __func__, __LINE__, ##__VA_ARGS__)

// Guard entry/exit into functions — easy to see stall bracket in log.
#define TEST_TRACE_ENTER()    TEST_TRACE("ENTER")
#define TEST_TRACE_EXIT(rv)   TEST_TRACE("EXIT rv=%d", (int)(rv))
#define TEST_TRACE_STEP(msg)  TEST_TRACE("STEP %s", (msg))
