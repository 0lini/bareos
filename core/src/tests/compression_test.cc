/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

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
#include "gtest/gtest.h"
#include "include/bareos.h"
#include "include/ch.h"
#include "lib/compression.h"

#include <cstring>
#include <string>
#include <vector>

#if defined(HAVE_ZSTD)
#  include <zstd.h>
#endif

namespace {
constexpr bool ZstdMagicIsStable() { return COMPRESS_ZSTD == 0x5A535444U; }

static_assert(ZstdMagicIsStable());
}  // namespace

TEST(compression, CompressorNameRecognizesZstd)
{ EXPECT_EQ(CompressorName(COMPRESS_ZSTD), "ZSTD"); }

#if defined(HAVE_ZSTD)
TEST(compression, ThreadlocalCompressRoundTripZstd)
{
  const std::string input(
      "Bareos ZSTD compression round-trip test payload. "
      "0123456789 repeating compressible text. "
      "0123456789 repeating compressible text. "
      "0123456789 repeating compressible text.");

  std::size_t capacity
      = RequiredCompressionOutputBufferSize(COMPRESS_ZSTD, input.size());
  // RequiredCompressionOutputBufferSize includes the stream header; the
  // thread-local compressor writes only the compressed payload.
  capacity = capacity > sizeof(comp_stream_header)
                 ? capacity - sizeof(comp_stream_header)
                 : capacity;
  std::vector<char> compressed(capacity);

  auto compressed_size
      = ThreadlocalCompress(COMPRESS_ZSTD, 3, input.data(), input.size(),
                            compressed.data(), compressed.size());
  ASSERT_FALSE(compressed_size.holds_error())
      << compressed_size.error_unchecked().c_str();
  EXPECT_GT(compressed_size.value_unchecked(), 0U);
  EXPECT_LT(compressed_size.value_unchecked(), input.size());

  std::vector<char> decompressed(input.size());
  size_t result
      = ZSTD_decompress(decompressed.data(), decompressed.size(),
                        compressed.data(), compressed_size.value_unchecked());
  ASSERT_FALSE(ZSTD_isError(result)) << ZSTD_getErrorName(result);
  ASSERT_EQ(result, input.size());
  EXPECT_EQ(std::string(decompressed.data(), result), input);
}

TEST(compression, ThreadlocalCompressZstdLevels)
{
  const char* input = "level-sensitive compressible sample data repeating ";
  std::size_t input_size = std::strlen(input) * 20;
  std::string payload;
  payload.reserve(input_size);
  while (payload.size() < input_size) { payload += input; }

  std::size_t capacity
      = RequiredCompressionOutputBufferSize(COMPRESS_ZSTD, payload.size());
  capacity = capacity > sizeof(comp_stream_header)
                 ? capacity - sizeof(comp_stream_header)
                 : capacity;

  std::vector<char> out_fast(capacity);
  std::vector<char> out_best(capacity);

  auto fast
      = ThreadlocalCompress(COMPRESS_ZSTD, 1, payload.data(), payload.size(),
                            out_fast.data(), out_fast.size());
  auto best = ThreadlocalCompress(COMPRESS_ZSTD, kZstdMaxConfiguredLevel,
                                  payload.data(), payload.size(),
                                  out_best.data(), out_best.size());

  ASSERT_FALSE(fast.holds_error()) << fast.error_unchecked().c_str();
  ASSERT_FALSE(best.holds_error()) << best.error_unchecked().c_str();
  EXPECT_LE(best.value_unchecked(), fast.value_unchecked());
  EXPECT_EQ(kZstdMaxConfiguredLevel,
            static_cast<std::uint32_t>(ZSTD_maxCLevel()));
}
#endif
