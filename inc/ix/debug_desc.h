#pragma once

#include <ix/stddef.h>
#include <ix/syscall.h>

#define LOG 0
#define LOG_TO_STDOUT 0
#define LOG_TO_SHMEM 0
#define LOG_DESC 0

#if LOG

DECLARE_PERCPU(int, poll_iteration);

#if LOG_DESC

void log_desc(const char *msg, int idx, bool usys, bool ret, const struct bsys_desc *desc);

#else

static inline void log_desc(const char *msg, int idx, bool usys, bool ret, const struct bsys_desc *desc) { }

#endif

void log_desc_msg(const char *msg, ...);
void debug_desc_init(void);
DECLARE_PERCPU(int, ix_log_context);
static inline void log_set_context(int ctx)
{
	percpu_get(ix_log_context) = ctx;
}

#else

static inline void log_desc(const char *msg, int idx, bool usys, bool ret, const struct bsys_desc *desc) { }
static inline void log_desc_msg(const char *msg, ...) { }
static inline void debug_desc_init(void) { }
static inline void log_set_context(int ctx) { }

#endif
