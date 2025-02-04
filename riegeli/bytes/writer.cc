// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/bytes/writer.h"

#include <stddef.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/arithmetic.h"
#include "riegeli/base/assert.h"
#include "riegeli/base/buffering.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/status.h"
#include "riegeli/base/types.h"
#include "riegeli/bytes/reader.h"

namespace riegeli {

void Writer::OnFail() { set_buffer(); }

absl::Status Writer::AnnotateStatusImpl(absl::Status status) {
  if (is_open()) return Annotate(status, absl::StrCat("at byte ", pos()));
  return status;
}

bool Writer::FailOverflow() {
  return Fail(absl::ResourceExhaustedError("Writer position overflow"));
}

// TODO: Optimize implementations below.
bool Writer::Write(float src) { return Write(absl::StrCat(src)); }

bool Writer::Write(double src) { return Write(absl::StrCat(src)); }

bool Writer::Write(long double src) {
  absl::Format(this, "%g",
               // Consistently use "nan", never "-nan".
               ABSL_PREDICT_FALSE(std::isnan(src))
                   ? std::numeric_limits<long double>::quiet_NaN()
                   : src);
  return ok();
}

bool Writer::WriteSlow(absl::string_view src) {
  RIEGELI_ASSERT_LT(available(), src.size())
      << "Failed precondition of Writer::WriteSlow(string_view): "
         "enough space available, use Write(string_view) instead";
  do {
    const size_t available_length = available();
    // `std::memcpy(nullptr, _, 0)` is undefined.
    if (available_length > 0) {
      std::memcpy(cursor(), src.data(), available_length);
      move_cursor(available_length);
      src.remove_prefix(available_length);
    }
    if (ABSL_PREDICT_FALSE(!PushSlow(1, src.size()))) return false;
  } while (src.size() > available());
  std::memcpy(cursor(), src.data(), src.size());
  move_cursor(src.size());
  return true;
}

bool Writer::WriteStringSlow(std::string&& src) {
  RIEGELI_ASSERT_GT(src.size(), kMaxBytesToCopy)
      << "Failed precondition of Writer::WriteStringSlow(): "
         "string too short, use Write() instead";
  if (PrefersCopying() || Wasteful(src.capacity(), src.size())) {
    return Write(absl::string_view(src));
  }
  AssertInitialized(src.data(), src.size());
  AssertInitialized(start(), start_to_cursor());
  return WriteSlow(Chain(std::move(src)));
}

bool Writer::WriteSlow(const Chain& src) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), src.size())
      << "Failed precondition of Writer::WriteSlow(Chain): "
         "enough space available, use Write(Chain) instead";
  for (const absl::string_view fragment : src.blocks()) {
    if (ABSL_PREDICT_FALSE(!Write(fragment))) return false;
  }
  return true;
}

bool Writer::WriteSlow(Chain&& src) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), src.size())
      << "Failed precondition of Writer::WriteSlow(Chain&&): "
         "enough space available, use Write(Chain&&) instead";
  // Not `std::move(src)`: forward to `WriteSlow(const Chain&)`.
  return WriteSlow(src);
}

bool Writer::WriteSlow(const absl::Cord& src) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), src.size())
      << "Failed precondition of Writer::WriteSlow(Cord): "
         "enough space available, use Write(Cord) instead";
  {
    const absl::optional<absl::string_view> flat = src.TryFlat();
    if (flat != absl::nullopt) {
      return Write(*flat);
    }
  }
  for (const absl::string_view fragment : src.Chunks()) {
    if (ABSL_PREDICT_FALSE(!Write(fragment))) return false;
  }
  return true;
}

bool Writer::WriteSlow(absl::Cord&& src) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), src.size())
      << "Failed precondition of Writer::WriteSlow(Cord&&): "
         "enough space available, use Write(Cord&&) instead";
  // Not `std::move(src)`: forward to `WriteSlow(const absl::Cord&)`.
  return WriteSlow(src);
}

bool Writer::WriteZerosSlow(Position length) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Writer::WriteZerosSlow(): "
         "enough space available, use WriteZeros() instead";
  while (length > available()) {
    const size_t available_length = available();
    // `std::memset(nullptr, _, 0)` is undefined.
    if (available_length > 0) {
      std::memset(cursor(), 0, available_length);
      move_cursor(available_length);
      length -= available_length;
    }
    if (ABSL_PREDICT_FALSE(!Push(1, SaturatingIntCast<size_t>(length)))) {
      return false;
    }
  }
  std::memset(cursor(), 0, IntCast<size_t>(length));
  move_cursor(IntCast<size_t>(length));
  return true;
}

bool Writer::WriteCharsSlow(Position length, char src) {
  RIEGELI_ASSERT_LT(UnsignedMin(available(), kMaxBytesToCopy), length)
      << "Failed precondition of Writer::WriteCharsSlow(): "
         "enough space available, use WriteChars() instead";
  if (src == '\0') return WriteZerosSlow(length);
  while (length > available()) {
    const size_t available_length = available();
    // `std::memset(nullptr, _, 0)` is undefined.
    if (available_length > 0) {
      std::memset(cursor(), src, available_length);
      move_cursor(available_length);
      length -= available_length;
    }
    if (ABSL_PREDICT_FALSE(!Push(1, SaturatingIntCast<size_t>(length)))) {
      return false;
    }
  }
  std::memset(cursor(), src, IntCast<size_t>(length));
  move_cursor(IntCast<size_t>(length));
  return true;
}

bool Writer::FlushImpl(FlushType flush_type) { return ok(); }

bool Writer::SeekSlow(Position new_pos) {
  RIEGELI_ASSERT_NE(new_pos, pos())
      << "Failed precondition of Writer::SeekSlow(): "
         "position unchanged, use Seek() instead";
  return Fail(absl::UnimplementedError("Writer::Seek() not supported"));
}

absl::optional<Position> Writer::SizeImpl() {
  Fail(absl::UnimplementedError("Writer::Size() not supported"));
  return absl::nullopt;
}

bool Writer::TruncateImpl(Position new_size) {
  return Fail(absl::UnimplementedError("Writer::Truncate() not supported"));
}

Reader* Writer::ReadModeImpl(Position initial_pos) {
  Fail(absl::UnimplementedError("Writer::ReadMode() not supported"));
  return nullptr;
}

namespace writer_internal {

void DeleteReader(Reader* reader) { delete reader; }

}  // namespace writer_internal

}  // namespace riegeli
