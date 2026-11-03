/* Copyright 2026 ALTESSA SOLUTIONS INC.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file. */

#ifndef RMDOC_H
#define RMDOC_H
#include <stddef.h>
#include <stdint.h>

#include "export.h"
#define RMDOC_VERSION_MAJOR 1
#define RMDOC_VERSION_MINOR 0
#define RMDOC_VERSION_PATCH 0

/* C23 API. Counted UTF-8 names may not contain NUL. All output arguments remain
 * unchanged on failure, except error. Passing NULL for error suppresses
 * details. Zero-initialized result structs may be freed. Do not copy owning
 * results. */
typedef enum rmd_status : uint32_t {
  RMD_OK,
  RMD_ERR_INVALID_ARGUMENT,
  RMD_ERR_OUT_OF_MEMORY,
  RMD_ERR_IO,
  RMD_ERR_INVALID_NAME,
  RMD_ERR_DUPLICATE_NAME,
  RMD_ERR_EMPTY_ARCHIVE,
  RMD_ERR_FILE_TOO_LARGE,
  RMD_ERR_ARCHIVE_TOO_LARGE,
  RMD_ERR_NOT_AN_ARCHIVE,
  RMD_ERR_MALFORMED_ARCHIVE,
  RMD_ERR_UNSUPPORTED_COMPRESSION,
  RMD_ERR_CHECKSUM_MISMATCH,
  RMD_ERR_ARCHIVE_FINISHED,
  RMD_ERR_TOO_MANY_ENTRIES,
  RMD_ERR_ENTRY_TOO_LARGE,
  RMD_ERR_TOTAL_SIZE_EXCEEDED,
  RMD_ERR_COMPRESSION_RATIO_EXCEEDED,
  RMD_ERR_COMPRESSED_ENTRY_TOO_LARGE,
  RMD_ERR_COMPRESSED_TOTAL_SIZE_EXCEEDED,
  RMD_ERR_DIRECTORY_TOO_LARGE,
  RMD_ERR_NAME_TOO_LONG,
  RMD_ERR_ZIP64_UNSUPPORTED,
  RMD_ERR_ENCRYPTED_ENTRY,
  RMD_ERR_SPANNED_ARCHIVE,
  RMD_ERR_CALLBACK
} rmd_status;

typedef enum rmd_compression : uint16_t {
  RMD_STORE = 0,
  RMD_DEFLATE = 8,
  RMD_DEFAULT_COMPRESSION = 65535
} rmd_compression;

typedef enum rmd_layout : uint32_t { RMD_DEVICE, RMD_PACKAGE } rmd_layout;

typedef struct {
  const char* bytes;
  size_t length;
} rmd_str;

typedef struct {
  const uint8_t* bytes;
  size_t size;
} rmd_bytes;

typedef struct {
  uint8_t* bytes;
  size_t size;
} rmd_buffer;

typedef struct {
  rmd_status code;
  uint64_t actual, limit;
  size_t
      name_length; /* Full UTF-8 byte length; name is a truncated diagnostic. */
  char name[256], message[192];
} rmd_error;

typedef struct {
  uint64_t maximum_entry_count, maximum_entry_size, maximum_total_size;
  uint64_t maximum_compression_ratio, ratio_threshold;
  uint64_t maximum_compressed_entry_size, maximum_compressed_total_size;
  uint64_t maximum_directory_size, maximum_name_length;
} rmd_limits;

typedef struct {
  rmd_str name;
  uint32_t compressed_size, uncompressed_size, crc32;
  uint16_t method;
} rmd_entry;

typedef struct {
  rmd_str name;
  rmd_bytes data;
} rmd_file;

typedef struct {
  const rmd_file* files;
  size_t count;
  void* _storage;
} rmd_archive;

typedef struct {
  const rmd_str* values;
  size_t count;
  void* _storage;
} rmd_strings;

typedef struct rmd_reader rmd_reader;
typedef struct rmd_writer rmd_writer;

/* Library version as "major.minor.patch"; the returned pointer is static. */
RMD_API const char* rmd_version(void);

/* Human-readable name for status; an out-of-range value gives "unknown
 * status". */
RMD_API const char* rmd_status_string(rmd_status status);

/* Generous default resource limits for rmd_reader_open. */
RMD_API rmd_limits rmd_limits_default(void);

/* Default limits with the compression ratio tightened to 1032:1, the
 * maximum deflate can produce; use to reject implausible archives. */
RMD_API rmd_limits rmd_limits_format(void);

/* Checks that name is valid UTF-8 and safe for layout: no control
 * characters, backslashes or Unicode format characters, and a path shape
 * (component count, no leading dot) that the layout permits. */
[[nodiscard]] RMD_API rmd_status rmd_validate_name(rmd_str name,
                                                   rmd_layout layout,
                                                   rmd_error* error);

/* Borrowed substring. No validation or allocation. */
RMD_API rmd_str rmd_document_id(rmd_str name);

/* Frees buffer's bytes and resets it to the empty state. No-op on NULL. */
RMD_API void rmd_buffer_free(rmd_buffer* buffer);

/* Frees every name and payload archive owns, then resets it to the empty
 * state. No-op on NULL. */
RMD_API void rmd_archive_free(rmd_archive* archive);

/* Frees every string strings owns, then resets it to the empty state.
 * No-op on NULL. */
RMD_API void rmd_strings_free(rmd_strings* strings);

/* Parses and validates the ENTIRE directory and local headers; no inflation.
 * Memory input is borrowed and MUST remain alive and unchanged until close.
 * File input is mapped (POSIX/Windows); keep the file unchanged until close.
 * NULL limits selects defaults. Reader snapshots the supplied limits. */
[[nodiscard]] RMD_API rmd_status rmd_reader_open(rmd_bytes data,
                                                 const rmd_limits* limits,
                                                 rmd_reader** out,
                                                 rmd_error* error);

[[nodiscard]] RMD_API rmd_status rmd_reader_open_file(const char* path,
                                                      const rmd_limits* limits,
                                                      rmd_reader** out,
                                                      rmd_error* error);

/* Closes reader, releasing its entries and, if it owns a mapped file,
 * unmapping it. No-op on NULL. */
RMD_API void rmd_reader_close(rmd_reader* reader);

/* Number of entries in the archive; zero if reader is NULL. */
RMD_API size_t rmd_reader_count(const rmd_reader* reader);

/* Entries/names are borrowed until close; out-of-range returns NULL. */
RMD_API const rmd_entry* rmd_reader_entry(const rmd_reader* reader,
                                          size_t index);

/* Collects the distinct document IDs (see rmd_document_id) across all
 * entries, in first-seen order. The result is owned; free with
 * rmd_strings_free. */
[[nodiscard]] RMD_API rmd_status rmd_reader_document_ids(
    const rmd_reader* reader, rmd_strings* out, rmd_error* error);

/* Callbacks return RMD_OK to continue; any other value becomes CALLBACK.
 * Selection runs for ALL entries before selected-only budgets and before body.
 * NULL selection selects all. Body payload is borrowed only during the call.
 * Do not close/mutate the reader or input from callbacks. No callbacks after
 * failure. */
typedef rmd_status (*rmd_select_fn)(void* context, const rmd_entry* entry,
                                    bool* include);

typedef rmd_status (*rmd_file_fn)(void* context, const rmd_entry* entry,
                                  rmd_bytes payload);

/* Invokes select then body for each entry that passes the budgets (see
 * rmd_select_fn and rmd_file_fn above); payload is decompressed and
 * checksum-verified before body sees it. */
[[nodiscard]] RMD_API rmd_status rmd_reader_foreach(
    const rmd_reader* reader, rmd_select_fn select, void* select_context,
    rmd_file_fn body, void* body_context, rmd_error* error);

/* Like rmd_reader_foreach, but collects the selected entries'
 * decompressed, checksum-verified contents into a newly allocated archive
 * instead of invoking a callback. */
[[nodiscard]] RMD_API rmd_status rmd_reader_unpack(const rmd_reader* reader,
                                                   rmd_select_fn select,
                                                   void* select_context,
                                                   rmd_archive* out,
                                                   rmd_error* error);

/* Stored, device-layout ZIP. Deterministic: the same files give the same bytes.
 */
[[nodiscard]] RMD_API rmd_status rmd_pack(const rmd_file* files, size_t count,
                                          rmd_buffer* out, rmd_error* error);

/* Opens/truncates a seekable file. Explicit finish is required. close never
 * finishes implicitly. Any failure after writing starts poisons the writer;
 * discard the partial file. Validation failures before writes are recoverable.
 * Use a staging path and rename after finish. One writer, one caller.
 * UTF-8 paths on both POSIX and Windows. */
[[nodiscard]] RMD_API rmd_status rmd_writer_open(const char* path,
                                                 rmd_compression compression,
                                                 rmd_layout layout,
                                                 rmd_writer** out,
                                                 rmd_error* error);

/* Deflate is used only if smaller, otherwise stored. DEFAULT uses writer mode.
 */
[[nodiscard]] RMD_API rmd_status rmd_writer_add(rmd_writer* writer,
                                                rmd_file file,
                                                rmd_compression compression,
                                                rmd_error* error);

/* File sources stream in bounded chunks and are always STORED. */
[[nodiscard]] RMD_API rmd_status rmd_writer_add_file(rmd_writer* writer,
                                                     rmd_str name,
                                                     const char* source_path,
                                                     rmd_error* error);

/* Writes the central directory and end record, then closes the output
 * file; the writer is marked finished either way. Call rmd_writer_close
 * afterward to release it. */
[[nodiscard]] RMD_API rmd_status rmd_writer_finish(rmd_writer* writer,
                                                   rmd_error* error);

/* Releases writer and its buffered records; if finish was never called,
 * closes the underlying file without completing the archive. No-op on
 * NULL. */
RMD_API void rmd_writer_close(rmd_writer* writer);
#endif
