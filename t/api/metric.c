/*
 * ProFTPD - mod_prometheus API testsuite
 * Copyright (c) 2021 TJ Saunders <tj@castaglia.org>
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

/* Metric API tests. */

#include "tests.h"
#include "prometheus/db.h"
#include "prometheus/metric.h"

static pool *p = NULL;
static const char *test_dir = "/tmp/prt-mod_prometheus-test-metrics";

static void set_up(void) {
  if (p == NULL) {
    p = permanent_pool = make_sub_pool(NULL);
  }

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("prometheus.db", 1, 20);
    pr_trace_set_levels("prometheus.metric", 1, 20);
    pr_trace_set_levels("prometheus.metric.db", 1, 20);
  }

  mark_point();
  prom_db_init(p);
}

static void tear_down(void) {
  if (getenv("TEST_VERBOSE") != NULL) {
    pr_trace_set_levels("prometheus.db", 0, 0);
    pr_trace_set_levels("prometheus.metric", 0, 0);
    pr_trace_set_levels("prometheus.metric.db", 0, 0);
  }

  prom_db_free();
  (void) tests_rmpath(p, test_dir);

  if (p != NULL) {
    destroy_pool(p);
    p = permanent_pool = NULL;
  }
}

START_TEST (metric_free_test) {
  int res;

  mark_point();
  res = prom_metric_free(NULL, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_free(p, NULL);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
}
END_TEST

START_TEST (metric_init_test) {
  int res;
  struct prom_dbh *dbh;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  dbh = prom_metric_init(NULL, NULL);
  fail_unless(dbh == NULL, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, NULL);
  fail_unless(dbh == NULL, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_destroy_test) {
  int res;

  mark_point();
  res = prom_metric_destroy(NULL, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_destroy(p, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);
}
END_TEST

START_TEST (metric_create_test) {
  int res;
  const char *name, *expected;
  struct prom_dbh *dbh;
  struct prom_metric *metric;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  metric = prom_metric_create(NULL, NULL, NULL);
  fail_unless(metric == NULL, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  metric = prom_metric_create(p, NULL, NULL);
  fail_unless(metric == NULL, "Failed to handle null name");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, NULL);
  fail_unless(metric == NULL, "Failed to handle null dbh");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  expected = "test";
  fail_unless(strcmp(prom_metric_get_name(metric), expected) == 0,
    "Expected metric name '%s', got '%s'", expected,
    prom_metric_get_name(metric));

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_add_counter_test) {
  int res;
  const char *name, *suffix;
  struct prom_dbh *dbh;
  struct prom_metric *metric;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_add_counter(NULL, NULL, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_counter(metric, NULL, NULL);
  fail_unless(res < 0, "Failed to handle null help");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  suffix = "total";
  res = prom_metric_add_counter(metric, suffix, "testing");
  fail_unless(res == 0, "Failed to add counter to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_add_gauge_test) {
  int res;
  const char *name, *suffix;
  struct prom_dbh *dbh;
  struct prom_metric *metric;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_add_gauge(NULL, NULL, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_gauge(metric, NULL, NULL);
  fail_unless(res < 0, "Failed to handle null help");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  suffix = "count";
  res = prom_metric_add_gauge(metric, suffix, "testing");
  fail_unless(res == 0, "Failed to add gauge to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_add_histogram_test) {
  int res;
  const char *name, *suffix;
  struct prom_dbh *dbh;
  struct prom_metric *metric;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_add_histogram(NULL, NULL, NULL, 0);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_histogram(metric, NULL, NULL, 0);
  fail_unless(res < 0, "Failed to handle null help");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  suffix = "weight";
  res = prom_metric_add_histogram(metric, suffix, "testing", 0);
  fail_unless(res == 0, "Failed to add histogram to metric: %s",
    strerror(errno));

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_set_dbh_test) {
  int res;
  struct prom_metric *metric;
  struct prom_dbh *dbh;

  mark_point();
  res = prom_metric_set_dbh(NULL, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  /* For purposes of testing, this does not have to be a real dbh. */
  mark_point();
  dbh = palloc(p, 8);
  metric = prom_metric_create(p, "test", dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_set_dbh(metric, NULL);
  fail_unless(res < 0, "Failed to handle null dbh");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_set_dbh(metric, dbh);
  fail_unless(res == 0, "Failed to set dbh: %s", strerror(errno));

  prom_metric_destroy(p, metric);
}
END_TEST

START_TEST (metric_get_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  results = prom_metric_get(NULL, NULL, 0, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  results = prom_metric_get(p, NULL, 0, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, -1, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle unknown metric type");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle counter-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle gauge-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_HISTOGRAM, NULL, NULL);
  fail_unless(results == NULL, "Failed to handle histogram-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_decr_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  uint32_t decr_val = 32;
  pr_table_t *labels;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_decr(NULL, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_decr(p, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_decr(p, metric, decr_val, NULL);
  fail_unless(res < 0, "Failed to handle gauge-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_add_gauge(metric, "count", "testing");
  fail_unless(res == 0, "Failed to add gauge to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_decr(p, metric, decr_val, NULL);
  fail_unless(res == 0, "Failed to decrement metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_decr(p, metric, decr_val, labels);
  fail_unless(res == 0, "Failed to decrement metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_incr_type_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  uint32_t incr_val = 66;
  pr_table_t *labels;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_incr_type(NULL, NULL, 0, NULL, 0);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_incr_type(p, NULL, 0, NULL, 0);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_incr_type(p, metric, incr_val, NULL, 0);
  fail_unless(res < 0, "Failed to handle unknown metric type");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_incr_type(p, metric, incr_val, NULL,
    PROM_METRIC_TYPE_COUNTER);
  fail_unless(res < 0, "Failed to handle counter-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_incr_type(p, metric, incr_val, NULL,
    PROM_METRIC_TYPE_GAUGE);
  fail_unless(res < 0, "Failed to handle gauge-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_add_counter(metric, "total", "testing");
  fail_unless(res == 0, "Failed to add counter to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_incr_type(p, metric, incr_val, NULL,
    PROM_METRIC_TYPE_COUNTER);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_incr_type(p, metric, incr_val, labels,
    PROM_METRIC_TYPE_COUNTER);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_incr_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  uint32_t incr_val = 66;
  pr_table_t *labels;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_incr(NULL, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_incr(p, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_incr(p, metric, incr_val, NULL);
  fail_unless(res < 0, "Failed to handle counter-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_add_counter(metric, "total", "testing");
  fail_unless(res == 0, "Failed to add counter to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_incr(p, metric, incr_val, NULL);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_incr(p, metric, incr_val, labels);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_incr_counter_gauge_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  uint32_t incr_val = 66;
  pr_table_t *labels;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_counter(metric, "total", "testing");
  fail_unless(res == 0, "Failed to add counter to metric: %s", strerror(errno));

  res = prom_metric_add_gauge(metric, "count", "testing");
  fail_unless(res == 0, "Failed to add gauge to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_incr(p, metric, incr_val, NULL);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_incr(p, metric, incr_val, labels);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_COUNTER, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled counter samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_observe_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  double observed_val = 3.1415;
  pr_table_t *labels;
  const array_header *results, *counts = NULL, *sums = NULL;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_observe(NULL, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_observe(p, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_observe(p, metric, observed_val, NULL);
  fail_unless(res < 0, "Failed to handle histogram-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_add_histogram(metric, "units", "testing", 0);
  fail_unless(res == 0, "Failed to add histogram to metric: %s",
    strerror(errno));

  mark_point();
  res = prom_metric_observe(p, metric, observed_val, NULL);
  fail_unless(res == 0, "Failed to observe metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_HISTOGRAM, &counts,
    &sums);
  fail_unless(results != NULL, "Failed get histogram results: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 bucket results, got %d",
    results->nelts);
  fail_unless(counts != NULL, "Failed get histogram count results: %s",
    strerror(errno));
  fail_unless(counts->nelts == 2, "Expected 2 count results, got %d",
    counts->nelts);
  fail_unless(sums != NULL, "Failed get histogram sum results: %s",
    strerror(errno));
  fail_unless(sums->nelts == 2, "Expected 2 sum results, got %d",
    sums->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_observe(p, metric, observed_val, labels);
  fail_unless(res == 0, "Failed to observe metric: %s", strerror(errno));

  mark_point();
  counts = sums = NULL;
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_HISTOGRAM, &counts,
    &sums);
  fail_unless(results != NULL, "Failed to get histogram results: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 bucket results, got %d",
    results->nelts);
  fail_unless(counts != NULL, "Failed to get histogram count results: %s",
    strerror(errno));
  fail_unless(counts->nelts == 4, "Expected 4 count results, got %d",
    counts->nelts);
  fail_unless(sums != NULL, "Failed to get histogram sum results: %s",
    strerror(errno));
  fail_unless(sums->nelts == 4, "Expected 4 sum results, got %d",
    sums->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_set_test) {
  int res;
  const char *name;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  uint32_t set_val = 42;
  pr_table_t *labels;
  const array_header *results;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  res = prom_metric_set(NULL, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null pool");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_set(p, NULL, 0, NULL);
  fail_unless(res < 0, "Failed to handle null metric");
  fail_unless(errno == EINVAL, "Expected EINVAL (%d), got %s (%d)", EINVAL,
    strerror(errno), errno);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_set(p, metric, set_val, NULL);
  fail_unless(res < 0, "Failed to handle gauge-less metric");
  fail_unless(errno == EPERM, "Expected EPERM (%d), got %s (%d)", EPERM,
    strerror(errno), errno);

  mark_point();
  res = prom_metric_add_gauge(metric, "count", "testing");
  fail_unless(res == 0, "Failed to add gauge to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_set(p, metric, set_val, NULL);
  fail_unless(res == 0, "Failed to set metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get label-less gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 2, "Expected 2 results, got %d",
    results->nelts);

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_set(p, metric, set_val, labels);
  fail_unless(res == 0, "Failed to set metric: %s", strerror(errno));

  mark_point();
  results = prom_metric_get(p, metric, PROM_METRIC_TYPE_GAUGE, NULL, NULL);
  fail_unless(results != NULL, "Failed to get labeled gauge samples: %s",
    strerror(errno));
  fail_unless(results->nelts == 4, "Expected 4 results, got %d",
    results->nelts);

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

START_TEST (metric_get_text_test) {
  int res;
  const char *name, *text;
  size_t textlen = 0;
  struct prom_dbh *dbh;
  struct prom_metric *metric;
  pr_table_t *labels;

  (void) tests_rmpath(p, test_dir);
  (void) tests_mkpath(p, test_dir);

  mark_point();
  dbh = prom_metric_init(p, test_dir);
  fail_unless(dbh != NULL, "Failed to init metrics: %s", strerror(errno));

  mark_point();
  name = "test";
  metric = prom_metric_create(p, name, dbh);
  fail_unless(metric != NULL, "Failed to create metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_counter(metric, "total", "counter testing");
  fail_unless(res == 0, "Failed to add counter to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_gauge(metric, "count", "gauge testing");
  fail_unless(res == 0, "Failed to add gauge to metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_add_histogram(metric, "weight", "histogram testing", 0);
  fail_unless(res == 0, "Failed to add histogram to metric: %s",
    strerror(errno));

  mark_point();
  res = prom_metric_incr(p, metric, 6, NULL);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  /* Now, provide labels. */
  labels = pr_table_nalloc(p, 0, 2);
  (void) pr_table_add_dup(labels, "protocol", "ftp", 0);
  (void) pr_table_add_dup(labels, "foo", "BAR", 0);

  mark_point();
  res = prom_metric_incr(p, metric, 8, labels);
  fail_unless(res == 0, "Failed to increment metric: %s", strerror(errno));

  mark_point();
  res = prom_metric_observe(p, metric, 76.42, labels);
  fail_unless(res == 0, "Failed to observe metric: %s", strerror(errno));

  mark_point();
  text = prom_metric_get_text(p, metric, "prt", &textlen);
  fail_unless(text != NULL, "Failed to get metric text: %s", strerror(errno));
  fail_unless(textlen > 0, "Expected text data, got %lu",
    (unsigned long) textlen);

  /* Use strstr(3) to assert bits of text. */
  fail_unless(strstr(text, "# HELP prt_test_total") != NULL,
    "Expected counter HELP text");
  fail_unless(strstr(text, "# TYPE prt_test_total counter") != NULL,
    "Expected counter TYPE text");
  fail_unless(strstr(text, "prt_test_total 6") != NULL,
    "Expected label-less counter sample");
  fail_unless(
    strstr(text, "prt_test_total{foo=\"BAR\",protocol=\"ftp\"} 8") != NULL,
    "Expected labeled counter sample");

  fail_unless(strstr(text, "# HELP prt_test_count") != NULL,
    "Expected gauge HELP text");
  fail_unless(strstr(text, "# TYPE prt_test_count gauge") != NULL,
    "Expected gauge TYPE text");
  fail_unless(strstr(text, "prt_test_count 6") != NULL,
    "Expected label-less gauge sample");
  fail_unless(
    strstr(text, "prt_test_count{foo=\"BAR\",protocol=\"ftp\"} 8") != NULL,
    "Expected labeled gauge sample");

  fail_unless(strstr(text, "# HELP prt_test_weight") != NULL,
    "Expected histogram HELP text");
  fail_unless(strstr(text, "# TYPE prt_test_weight histogram") != NULL,
    "Expected histogram TYPE text");
  fail_unless(
    strstr(text, "prt_test_weight_bucket{foo=\"BAR\",le=\"+Inf\",protocol=\"ftp\"} 1") != NULL,
    "Expected labeled histogram bucket sample");
  fail_unless(
    strstr(text, "prt_test_weight_count{foo=\"BAR\",protocol=\"ftp\"} 1") != NULL,
    "Expected labeled histogram count sample");
  fail_unless(
    strstr(text, "prt_test_weight_sum{foo=\"BAR\",protocol=\"ftp\"} 76.42") != NULL,
    "Expected labeled histogram sum sample");

  mark_point();
  res = prom_metric_destroy(p, metric);
  fail_unless(res == 0, "Failed to destroy metric: %s", strerror(errno));

  res = prom_metric_free(p, dbh);
  fail_unless(res == 0, "Failed to free metrics: %s", strerror(errno));
  (void) tests_rmpath(p, test_dir);
}
END_TEST

Suite *tests_get_metric_suite(void) {
  Suite *suite;
  TCase *testcase;

  suite = suite_create("metric");
  testcase = tcase_create("base");

  tcase_add_checked_fixture(testcase, set_up, tear_down);

  tcase_add_test(testcase, metric_free_test);
  tcase_add_test(testcase, metric_init_test);

  tcase_add_test(testcase, metric_destroy_test);
  tcase_add_test(testcase, metric_create_test);

  tcase_add_test(testcase, metric_add_counter_test);
  tcase_add_test(testcase, metric_add_gauge_test);
  tcase_add_test(testcase, metric_add_histogram_test);
  tcase_add_test(testcase, metric_set_dbh_test);

  tcase_add_test(testcase, metric_get_test);
  tcase_add_test(testcase, metric_decr_test);
  tcase_add_test(testcase, metric_incr_type_test);
  tcase_add_test(testcase, metric_incr_test);
  tcase_add_test(testcase, metric_incr_counter_gauge_test);
  tcase_add_test(testcase, metric_observe_test);
  tcase_add_test(testcase, metric_set_test);

  tcase_add_test(testcase, metric_get_text_test);

  suite_add_tcase(suite, testcase);
  return suite;
}
