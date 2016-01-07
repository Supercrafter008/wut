#pragma once
#include <wut.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t GX2InitAttributes;

enum GX2InitAttributes
{
	GX2_INIT_END = 0,
	GX2_INIT_CMD_BUF_BASE = 1,
	GX2_INIT_CMD_BUF_POOL_SIZE = 2,
	GX2_INIT_ARGC = 7,
	GX2_INIT_ARGV = 8,
};

void
GX2Init(uint32_t *attributes);

void
GX2Shutdown();

#ifdef __cplusplus
}
#endif
