#ifndef FMC16_SMOKE_H
#define FMC16_SMOKE_H

#include <stdint.h>

int fmc16_minimal_smoke_run(void);
int fmc16_minimal_smoke_run_with_capabilities(uint32_t *peer_capabilities);

#endif
