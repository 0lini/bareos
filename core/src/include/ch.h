/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
// Laurent Papier
/**
 * @file
 * Compressed stream header struct
 */

#ifndef BAREOS_INCLUDE_CH_H_
#define BAREOS_INCLUDE_CH_H_

#include <cstdint>
#include <cstddef>

namespace {
constexpr std::uint32_t compression_constant(const char (&txt)[5])
{
  std::uint32_t a32 = txt[0];
  std::uint32_t b32 = txt[1];
  std::uint32_t c32 = txt[2];
  std::uint32_t d32 = txt[3];

  return (a32 << 24) | (b32 << 16) | (c32 << 8) | d32;
}
};  // namespace

// Compression algorithm signature. 4 letters as a 32bits integer
enum compression_type : std::uint32_t
{
  COMPRESS_NONE
  = compression_constant("NONE"), /* used for incompressible block */
  COMPRESS_GZIP = compression_constant("GZIP"),
  COMPRESS_LZO1X = compression_constant("LZOX"),
  COMPRESS_FZFZ = compression_constant("FZFZ"),
  COMPRESS_FZ4L = compression_constant("FZ4L"),
  COMPRESS_FZ4H = compression_constant("FZ4H"),
  COMPRESS_ZSTD = compression_constant("ZSTD"),
};

// double check our constants with the previously defined values
static_assert(0x4e4f4e45 == compression_constant("NONE"));
static_assert(0x475a4950 == compression_constant("GZIP"));
static_assert(0x4c5a4f58 == compression_constant("LZOX"));
static_assert(0x465A465A == compression_constant("FZFZ"));
static_assert(0x465A344C == compression_constant("FZ4L"));
static_assert(0x465A3448 == compression_constant("FZ4H"));
static_assert(0x5A535444 == compression_constant("ZSTD"));

// FileSet-configurable ZSTD levels are 1..kZstdMaxConfiguredLevel.
// This matches libzstd's ZSTD_maxCLevel() (22) as of zstd 1.5.x.
constexpr std::uint32_t kZstdMaxConfiguredLevel = 22;

/* Parse one or more decimal digits as a ZSTD FileSet level.
 * On entry, *pp must point at the first digit after "Zs".
 * On success, *pp points at the last digit consumed.
 * Returns false if missing or outside 1..kZstdMaxConfiguredLevel. */
constexpr bool ParseZstdConfiguredLevel(const char** pp, std::uint32_t* level)
{
  const char* p = *pp;
  if (*p < '0' || *p > '9') { return false; }

  std::uint32_t value = 0;
  const char* last = p;
  while (*p >= '0' && *p <= '9') {
    std::uint32_t digit_value = static_cast<std::uint32_t>(*p - '0');
    if (value > (kZstdMaxConfiguredLevel - digit_value) / 10) { return false; }
    value = value * 10 + digit_value;
    last = p;
    ++p;
  }

  if (value < 1 || value > kZstdMaxConfiguredLevel) { return false; }

  *level = value;
  *pp = last;
  return true;
}

namespace {
constexpr bool TestParseZstdConfiguredLevel(const char* digits,
                                            std::uint32_t expected)
{
  const char* p = digits;
  std::uint32_t level = 0;
  if (!ParseZstdConfiguredLevel(&p, &level)) { return false; }
  if (level != expected) { return false; }

  std::size_t len = 0;
  while (digits[len] != '\0') { ++len; }
  // On success, p points at the last digit consumed.
  return len > 0 && p == digits + len - 1;
}

constexpr bool TestParseZstdLevelRejects(const char* digits)
{
  const char* p = digits;
  std::uint32_t level = 0;
  return !ParseZstdConfiguredLevel(&p, &level);
}

static_assert(TestParseZstdConfiguredLevel("1", 1));
static_assert(TestParseZstdConfiguredLevel("3", 3));
static_assert(TestParseZstdConfiguredLevel("9", 9));
static_assert(TestParseZstdConfiguredLevel("10", 10));
static_assert(TestParseZstdConfiguredLevel("22", 22));
static_assert(TestParseZstdLevelRejects(""));
static_assert(TestParseZstdLevelRejects("0"));
static_assert(TestParseZstdLevelRejects("23"));
static_assert(TestParseZstdLevelRejects("31"));
static_assert(TestParseZstdLevelRejects("100"));
}  // namespace

// Compression header version
#define COMP_HEAD_VERSION 0x1

/* Compressed data stream header */
typedef struct {
  uint32_t magic;   /* compression algo used in this compressed data stream */
  uint16_t level;   /* compression level used */
  uint16_t version; /* for futur evolution */
  uint32_t size;    /* compressed size of the original data */
} comp_stream_header;

#endif  // BAREOS_INCLUDE_CH_H_
