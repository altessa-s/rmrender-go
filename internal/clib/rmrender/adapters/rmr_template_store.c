/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmr_files.h"

struct rmr_template_store {
  char** directories;
  size_t count;
  rmr_template_document* document;
  rmr_limits limits;
};

/* Free a template store, its directory copies and cached model; s may be
 * null. */
void rmr_template_store_free(rmr_template_store* s) {
  if (s) {
    for (size_t i = 0; i < s->count; i++) free(s->directories[i]);
    free(s->directories);
    rmr_template_document_free(s->document);
    free(s);
  }
}

rmr_status rmr_template_store_create_layered(const char* const* directories,
                                             size_t count,
                                             const rmr_limits* given,
                                             rmr_template_store** out,
                                             rmr_error* error) {
  rmr_status status = RMR_OK;
  rmr_limits l = given ? *given : rmr_limits_default();
  if ((!directories && count) || !out)
    status = RMR_ERR_INVALID_ARGUMENT;
  else if (count > l.maximum_elements ||
           sizeof(rmr_template_store) > l.maximum_plan_bytes ||
           count > (l.maximum_plan_bytes - sizeof(rmr_template_store)) /
                       sizeof(char*))
    status = RMR_ERR_LIMIT;

  rmr_template_store* s = nullptr;
  if (status == RMR_OK) {
    s = calloc(1, sizeof *s);
    if (!s) status = RMR_ERR_OUT_OF_MEMORY;
  }

  if (status == RMR_OK) {
    s->limits = l;
    s->limits.maximum_plan_bytes -= sizeof *s + count * sizeof(char*);
    s->directories = count ? calloc(count, sizeof(char*)) : nullptr;
    if (count && !s->directories) status = RMR_ERR_OUT_OF_MEMORY;
  }

  for (size_t i = 0; i < count && status == RMR_OK; i++) {
    if (!directories[i]) {
      status = RMR_ERR_INVALID_ARGUMENT;
      break;
    }
    size_t length = strlen(directories[i]);
    if (length >= s->limits.maximum_plan_bytes) {
      status = RMR_ERR_LIMIT;
      break;
    }
    s->directories[i] = malloc(length + 1);
    if (!s->directories[i]) {
      status = RMR_ERR_OUT_OF_MEMORY;
      break;
    }
    memcpy(s->directories[i], directories[i], length + 1);
    s->count++;
    s->limits.maximum_plan_bytes -= length + 1;
  }

  if (status == RMR_OK) {
    *out = s;
    return RMR_OK;
  }

  rmr_template_store_free(s);
  if (error) {
    *error = (rmr_error){.code = status};
    snprintf(error->message, sizeof error->message,
             "template store creation failed");
  }
  return status;
}

rmr_status rmr_template_store_create(const char* directory,
                                     const rmr_limits* given,
                                     rmr_template_store** out,
                                     rmr_error* error) {
  return rmr_template_store_create_layered(&directory, 1, given, out, error);
}

/* rmr_template_resolve_fn callback: finds name's .template file across the
 * store's layered directories, decodes it, and caches the decoded model
 * (replacing any previously resolved one) for the duration of this store. */
rmr_status rmr_template_store_resolve(void* context, rm_str name,
                                      const rmr_template** out) {
  rmr_template_store* s = context;
  if (!s || !out || !name.bytes || !name.length || name.bytes[0] == '.' ||
      memchr(name.bytes, 0, name.length) ||
      memchr(name.bytes, '/', name.length) ||
      memchr(name.bytes, '\\', name.length))
    return RMR_ERR_INVALID_ARGUMENT;

  rmr_buffer data = {0};
  rmr_status status = RMR_OK;
  for (size_t i = 0; i < s->count; i++) {
    status =
        rmr_read_resource(s->directories[i], name.bytes, name.length,
                          ".template", s->limits.maximum_plan_bytes, &data);
    if (status != RMR_OK) return status;
    if (data.bytes) break;
  }

  if (!data.bytes) {
    *out = nullptr;
    return RMR_OK;
  }

  rmr_template_document_free(s->document);
  s->document = nullptr;
  rmr_limits l = s->limits;
  l.maximum_plan_bytes -= data.size;
  status = rmr_template_document_decode((const char*)data.bytes, data.size, &l,
                                        &s->document, nullptr);
  rmr_buffer_free(&data);
  if (status == RMR_OK) *out = rmr_template_document_model(s->document);
  return status;
}
