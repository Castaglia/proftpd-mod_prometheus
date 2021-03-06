/*
 * ProFTPD - mod_prometheus http API
 * Copyright (c) 2021 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 */

#ifndef MOD_PROMETHEUS_HTTP_H
#define MOD_PROMETHEUS_HTTP_H

#include "mod_prometheus.h"
#include "prometheus/registry.h"

struct prom_http;

struct prom_http *prom_http_start(pool *p, const pr_netaddr_t *addr,
  struct prom_registry *registry, const char *username, const char *password);

/* This function will exit once the exporter finishes. */
int prom_http_run_loop(pool *p, struct prom_http *http);

int prom_http_stop(pool *p, struct prom_http *http);

int prom_http_init(pool *p);
int prom_http_free(void);

#endif /* MOD_PROMETHEUS_HTTP_H */
