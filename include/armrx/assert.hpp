#pragma once

// ARMRX_ASSERT — always-evaluated assertion macro.
//
// In debug builds (NDEBUG not defined): acts like plain assert() — aborts on failure.
// In release builds (NDEBUG defined): checks the condition and logs a warning
// on failure instead of aborting. Unlike plain assert(), this never silently
// compiles out, so input validation works in production.
//
// Hot-path note: the check is always evaluated. On slow paths (dataset init,
// config parsing) the cost is negligible. On hot paths (aes_hash, per-hash VM),
// the check is a single branch that should be well-predicted; if profiling shows
// measurable cost, gate specific calls behind `#ifndef ARMRX_RELEASE_CHECKS`.

#include <cstdio>
#include <cstdlib>

#ifndef ARMRX_ASSERT
#  ifndef NDEBUG
     // Debug: hard abort like plain assert()
#    define ARMRX_ASSERT(cond, msg) \
       do { \
         if (!(cond)) { \
           std::fprintf(stderr, "ARMRX_ASSERT FAILED [%s:%d]: %s — %s\n", \
                        __FILE__, __LINE__, #cond, (msg)); \
           std::abort(); \
         } \
       } while (false)
#  else
     // Release: log and skip instead of aborting
#    define ARMRX_ASSERT(cond, msg) \
       do { \
         if (!(cond)) { \
           std::fprintf(stderr, "ARMRX_ASSERT [%s:%d]: %s — %s\n", \
                        __FILE__, __LINE__, #cond, (msg)); \
         } \
       } while (false)
#  endif
#endif
