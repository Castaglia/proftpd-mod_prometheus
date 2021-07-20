/*
 * ProFTPD - mod_prometheus metrics datastore API
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

#ifndef MOD_PROMETHEUS_METRIC_DB_H
#define MOD_PROMETHEUS_METRIC_DB_H

#include "mod_prometheus.h"
#include "prometheus/db.h"

int prom_metric_db_close(pool *p, struct prom_dbh *dbh);
struct prom_dbh *prom_metric_db_open(pool *p, const char *tables_path);
struct prom_dbh *prom_metric_db_init(pool *p, const char *tables_path,
  int flags);

int prom_metric_db_add_metric(pool *p, struct prom_dbh *dbh,
  const char *metric_name);
int prom_metric_db_exists_metric(pool *p, struct prom_dbh *dbh,
  const char *metric_name);

#endif /* MOD_PROMETHEUS_METRIC_DB_H */
