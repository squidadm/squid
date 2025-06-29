/*
 * Copyright (C) 1996-2025 The Squid Software Foundation and contributors
 *
 * Squid software is distributed under GPLv2+ license and includes
 * contributions from numerous individuals and organizations.
 * Please see the COPYING and CONTRIBUTORS files for details.
 */

/* DEBUG: section 80    WCCP Support */

#ifndef SQUID_SRC_WCCP2_H
#define SQUID_SRC_WCCP2_H

#if USE_WCCPv2

class StoreEntry;

void parse_wccp2_method(int *v);
void free_wccp2_method(int *v);
void dump_wccp2_method(StoreEntry * e, const char *label, int v);
void parse_wccp2_amethod(int *v);
void free_wccp2_amethod(int *v);
void dump_wccp2_amethod(StoreEntry * e, const char *label, int v);

void parse_wccp2_service(void *v);
void free_wccp2_service(void *v);
void dump_wccp2_service(StoreEntry * e, const char *label, void *v);

int check_null_wccp2_service(void *v);

void parse_wccp2_service_info(void *v);

void free_wccp2_service_info(void *v);

void dump_wccp2_service_info(StoreEntry * e, const char *label, void *v);
#endif /* USE_WCCPv2 */

#endif /* SQUID_SRC_WCCP2_H */

