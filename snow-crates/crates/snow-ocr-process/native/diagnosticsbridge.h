// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_DIAGNOSTICS_BRIDGE_H
#define SNOW_DIAGNOSTICS_BRIDGE_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
int snow_diag_start(const char* handler, const char* database, const char* session,
                    const char* version, const char* revision);
int snow_diag_attach(const char* pipe, const char* session, const char* version);
void snow_diag_prepare(const char* session, const char* version, const char* revision);
const char* snow_diag_pipe(void);
#ifdef __APPLE__
int snow_diag_healthy(void);
#endif
void snow_diag_open_emergency(const char* path);
void snow_diag_emergency(const char* record, size_t length);
void snow_diag_fatal(const char* event);
void snow_diag_breadcrumb(const char* record, size_t length);
void snow_diag_panic(const unsigned char* location, size_t length);
void snow_diag_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
