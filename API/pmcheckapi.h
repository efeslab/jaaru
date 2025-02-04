/** @file pmcheckapi.h
 *  @brief C interface to the model checker.
 */

#ifndef PMCHECKAPI_H
#define PMCHECKAPI_H

#include <stdint.h>

#if __cplusplus
extern "C" {
#else
typedef int bool;
#endif
uint8_t pmc_load8(void *addr, const char * position);
uint16_t pmc_load16(void *addr, const char * position);
uint32_t pmc_load32(void *addr, const char * position);
uint64_t pmc_load64(void *addr, const char * position);

void pmc_store8(void *addr, uint8_t val, const char * position);
void pmc_store16(void *addr, uint16_t val, const char * position);
void pmc_store32(void *addr, uint32_t val, const char * position);
void pmc_store64(void *addr, uint64_t val, const char * position);

#if __cplusplus
}
#endif

#endif
