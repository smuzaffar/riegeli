// Copyright 2021 Google LLC
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

#ifndef RIEGELI_BYTES_POSITION_SHIFTING_WRITER_H_
#define RIEGELI_BYTES_POSITION_SHIFTING_WRITER_H_

#include <stddef.h>

#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/arithmetic.h"
#include "riegeli/base/assert.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/base/types.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

template <typename Src>
class PositionShiftingReader;
class Reader;

// Template parameter independent part of `PositionShiftingWriter`.
class PositionShiftingWriterBase : public Writer {
 public:
  class Options {
   public:
    Options() noexcept {}

    // The base position of the new `Writer`.
    //
    // Default: 0.
    Options& set_base_pos(Position base_pos) & {
      base_pos_ = base_pos;
      return *this;
    }
    Options&& set_base_pos(Position base_pos) && {
      return std::move(set_base_pos(base_pos));
    }
    Position base_pos() const { return base_pos_; }

   private:
    Position base_pos_ = 0;
  };

  // Returns the new `Writer`. Unchanged by `Close()`.
  virtual Writer* DestWriter() = 0;
  virtual const Writer* DestWriter() const = 0;

  // Returns the base position of the original `Writer`.
  Position base_pos() const { return base_pos_; }

  bool PrefersCopying() const override;
  bool SupportsRandomAccess() override;
  bool SupportsTruncate() override;
  bool SupportsReadMode() override;

 protected:
  explicit PositionShiftingWriterBase(Closed) noexcept : Writer(kClosed) {}

  explicit PositionShiftingWriterBase(Position base_pos);

  PositionShiftingWriterBase(PositionShiftingWriterBase&& that) noexcept;
  PositionShiftingWriterBase& operator=(
      PositionShiftingWriterBase&& that) noexcept;

  void Reset(Closed);
  void Reset(Position base_pos);
  void Initialize(Writer* dest);
  ABSL_ATTRIBUTE_COLD absl::Status AnnotateOverDest(absl::Status status);

  // Sets cursor of `dest` to cursor of `*this`.
  void SyncBuffer(Writer& dest);

  // Sets buffer pointers of `*this` to buffer pointers of `dest`, adjusting
  // `start()` to hide data already written. Fails `*this` if `dest` failed.
  void MakeBuffer(Writer& dest);

  void Done() override;
  ABSL_ATTRIBUTE_COLD absl::Status AnnotateStatusImpl(
      absl::Status status) override;
  bool PushSlow(size_t min_length, size_t recommended_length) override;
  using Writer::WriteSlow;
  bool WriteSlow(absl::string_view src) override;
  bool WriteSlow(const Chain& src) override;
  bool WriteSlow(Chain&& src) override;
  bool WriteSlow(const absl::Cord& src) override;
  bool WriteSlow(absl::Cord&& src) override;
  bool WriteZerosSlow(Position length) override;
  bool SeekSlow(Position new_pos) override;
  absl::optional<Position> SizeImpl() override;
  bool TruncateImpl(Position new_size) override;
  Reader* ReadModeImpl(Position initial_pos) override;

 private:
  ABSL_ATTRIBUTE_COLD bool FailUnderflow(Position new_pos, Object& object);

  // This template is defined and used only in position_shifting_writer.cc.
  template <typename Src>
  bool WriteInternal(Src&& src);

  Position base_pos_ = 0;

  AssociatedReader<PositionShiftingReader<Reader*>> associated_reader_;

  // Invariants if `ok()`:
  //   `start() == DestWriter()->cursor()`
  //   `limit() == DestWriter()->limit()`
  //   `start_pos() == DestWriter()->pos() + base_pos_`
};

// A `Writer` which writes to another `Writer`, reporting positions shifted so
// that the beginning appears as the given base position. Seeking back before
// the base position fails.
//
// `PrefixLimitingWriter` can be used for shifting positions in the other
// direction.
//
// The `Dest` template parameter specifies the type of the object providing and
// possibly owning the original `Writer`. `Dest` must support
// `Dependency<Writer*, Dest>`, e.g. `Writer*` (not owned, default),
// `ChainWriter<>` (owned), `std::unique_ptr<Writer>` (owned),
// `AnyDependency<Writer*>` (maybe owned).
//
// By relying on CTAD the template argument can be deduced as the value type of
// the first constructor argument. This requires C++17.
//
// The original `Writer` must not be accessed until the `PositionShiftingWriter`
// is closed or no longer used, except that it is allowed to read the
// destination of the original `Writer` immediately after `Flush()`.
template <typename Dest = Writer*>
class PositionShiftingWriter : public PositionShiftingWriterBase {
 public:
  // Creates a closed `PositionShiftingWriter`.
  explicit PositionShiftingWriter(Closed) noexcept
      : PositionShiftingWriterBase(kClosed) {}

  // Will write to the original `Writer` provided by `dest`.
  explicit PositionShiftingWriter(const Dest& dest,
                                  Options options = Options());
  explicit PositionShiftingWriter(Dest&& dest, Options options = Options());

  // Will write to the original `Writer` provided by a `Dest` constructed from
  // elements of `dest_args`. This avoids constructing a temporary `Dest` and
  // moving from it.
  template <typename... DestArgs>
  explicit PositionShiftingWriter(std::tuple<DestArgs...> dest_args,
                                  Options options = Options());

  PositionShiftingWriter(PositionShiftingWriter&& that) noexcept;
  PositionShiftingWriter& operator=(PositionShiftingWriter&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `PositionShiftingWriter`.
  // This avoids constructing a temporary `PositionShiftingWriter` and moving
  // from it.
  ABSL_ATTRIBUTE_REINITIALIZES void Reset(Closed);
  ABSL_ATTRIBUTE_REINITIALIZES void Reset(const Dest& dest,
                                          Options options = Options());
  ABSL_ATTRIBUTE_REINITIALIZES void Reset(Dest&& dest,
                                          Options options = Options());
  template <typename... DestArgs>
  ABSL_ATTRIBUTE_REINITIALIZES void Reset(std::tuple<DestArgs...> dest_args,
                                          Options options = Options());

  // Returns the object providing and possibly owning the original `Writer`.
  // Unchanged by `Close()`.
  Dest& dest() { return dest_.manager(); }
  const Dest& dest() const { return dest_.manager(); }
  Writer* DestWriter() override { return dest_.get(); }
  const Writer* DestWriter() const override { return dest_.get(); }

 protected:
  void Done() override;
  void SetWriteSizeHintImpl(absl::optional<Position> write_size_hint) override;
  bool FlushImpl(FlushType flush_type) override;

 private:
  // Moves `that.dest_` to `dest_`. Buffer pointers are already moved from
  // `dest_` to `*this`; adjust them to match `dest_`.
  void MoveDest(PositionShiftingWriter&& that);

  // The object providing and possibly owning the original `Writer`.
  Dependency<Writer*, Dest> dest_;
};

// Support CTAD.
#if __cpp_deduction_guides
explicit PositionShiftingWriter(Closed)
    -> PositionShiftingWriter<DeleteCtad<Closed>>;
template <typename Dest>
explicit PositionShiftingWriter(const Dest& dest,
                                PositionShiftingWriterBase::Options options =
                                    PositionShiftingWriterBase::Options())
    -> PositionShiftingWriter<std::decay_t<Dest>>;
template <typename Dest>
explicit PositionShiftingWriter(Dest&& dest,
                                PositionShiftingWriterBase::Options options =
                                    PositionShiftingWriterBase::Options())
    -> PositionShiftingWriter<std::decay_t<Dest>>;
template <typename... DestArgs>
explicit PositionShiftingWriter(std::tuple<DestArgs...> dest_args,
                                PositionShiftingWriterBase::Options options =
                                    PositionShiftingWriterBase::Options())
    -> PositionShiftingWriter<DeleteCtad<std::tuple<DestArgs...>>>;
#endif

// Implementation details follow.

inline PositionShiftingWriterBase::PositionShiftingWriterBase(Position base_pos)
    : base_pos_(base_pos) {}

inline PositionShiftingWriterBase::PositionShiftingWriterBase(
    PositionShiftingWriterBase&& that) noexcept
    : Writer(static_cast<Writer&&>(that)),
      base_pos_(that.base_pos_),
      associated_reader_(std::move(that.associated_reader_)) {}

inline PositionShiftingWriterBase& PositionShiftingWriterBase::operator=(
    PositionShiftingWriterBase&& that) noexcept {
  Writer::operator=(static_cast<Writer&&>(that));
  base_pos_ = that.base_pos_;
  associated_reader_ = std::move(that.associated_reader_);
  return *this;
}

inline void PositionShiftingWriterBase::Reset(Closed) {
  Writer::Reset(kClosed);
  base_pos_ = 0;
  associated_reader_.Reset();
}

inline void PositionShiftingWriterBase::Reset(Position base_pos) {
  Writer::Reset();
  base_pos_ = base_pos;
  associated_reader_.Reset();
}

inline void PositionShiftingWriterBase::Initialize(Writer* dest) {
  RIEGELI_ASSERT(dest != nullptr)
      << "Failed precondition of PositionShiftingWriter: null Writer pointer";
  MakeBuffer(*dest);
}

inline void PositionShiftingWriterBase::SyncBuffer(Writer& dest) {
  dest.set_cursor(cursor());
}

inline void PositionShiftingWriterBase::MakeBuffer(Writer& dest) {
  if (ABSL_PREDICT_FALSE(dest.pos() >
                         std::numeric_limits<Position>::max() - base_pos_)) {
    FailOverflow();
    return;
  }
  set_buffer(dest.cursor(), dest.available());
  set_start_pos(dest.pos() + base_pos_);
  if (ABSL_PREDICT_FALSE(!dest.ok())) {
    FailWithoutAnnotation(AnnotateOverDest(dest.status()));
  }
}

template <typename Dest>
inline PositionShiftingWriter<Dest>::PositionShiftingWriter(const Dest& dest,
                                                            Options options)
    : PositionShiftingWriterBase(options.base_pos()), dest_(dest) {
  Initialize(dest_.get());
}

template <typename Dest>
inline PositionShiftingWriter<Dest>::PositionShiftingWriter(Dest&& dest,
                                                            Options options)
    : PositionShiftingWriterBase(options.base_pos()), dest_(std::move(dest)) {
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline PositionShiftingWriter<Dest>::PositionShiftingWriter(
    std::tuple<DestArgs...> dest_args, Options options)
    : PositionShiftingWriterBase(options.base_pos()),
      dest_(std::move(dest_args)) {
  Initialize(dest_.get());
}

template <typename Dest>
inline PositionShiftingWriter<Dest>::PositionShiftingWriter(
    PositionShiftingWriter&& that) noexcept
    : PositionShiftingWriterBase(
          static_cast<PositionShiftingWriterBase&&>(that)) {
  MoveDest(std::move(that));
}

template <typename Dest>
inline PositionShiftingWriter<Dest>& PositionShiftingWriter<Dest>::operator=(
    PositionShiftingWriter&& that) noexcept {
  PositionShiftingWriterBase::operator=(
      static_cast<PositionShiftingWriterBase&&>(that));
  MoveDest(std::move(that));
  return *this;
}

template <typename Dest>
inline void PositionShiftingWriter<Dest>::Reset(Closed) {
  PositionShiftingWriterBase::Reset(kClosed);
  dest_.Reset();
}

template <typename Dest>
inline void PositionShiftingWriter<Dest>::Reset(const Dest& dest,
                                                Options options) {
  PositionShiftingWriterBase::Reset(options.base_pos());
  dest_.Reset(dest);
  Initialize(dest_.get());
}

template <typename Dest>
inline void PositionShiftingWriter<Dest>::Reset(Dest&& dest, Options options) {
  PositionShiftingWriterBase::Reset(options.base_pos());
  dest_.Reset(std::move(dest));
  Initialize(dest_.get());
}

template <typename Dest>
template <typename... DestArgs>
inline void PositionShiftingWriter<Dest>::Reset(
    std::tuple<DestArgs...> dest_args, Options options) {
  PositionShiftingWriterBase::Reset(options.base_pos());
  dest_.Reset(std::move(dest_args));
  Initialize(dest_.get());
}

template <typename Dest>
inline void PositionShiftingWriter<Dest>::MoveDest(
    PositionShiftingWriter&& that) {
  if (dest_.kIsStable || that.dest_ == nullptr) {
    dest_ = std::move(that.dest_);
  } else {
    // Buffer pointers are already moved so `SyncBuffer()` is called on `*this`,
    // `dest_` is not moved yet so `dest_` is taken from `that`.
    SyncBuffer(*that.dest_);
    dest_ = std::move(that.dest_);
    MakeBuffer(*dest_);
  }
}

template <typename Dest>
void PositionShiftingWriter<Dest>::Done() {
  PositionShiftingWriterBase::Done();
  if (dest_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!dest_->Close())) {
      FailWithoutAnnotation(AnnotateOverDest(dest_->status()));
    }
  }
}

template <typename Dest>
void PositionShiftingWriter<Dest>::SetWriteSizeHintImpl(
    absl::optional<Position> write_size_hint) {
  if (dest_.is_owning()) {
    SyncBuffer(*dest_);
    dest_->SetWriteSizeHint(
        write_size_hint == absl::nullopt
            ? absl::nullopt
            : absl::make_optional(SaturatingAdd(base_pos(), *write_size_hint)));
    MakeBuffer(*dest_);
  }
}

template <typename Dest>
bool PositionShiftingWriter<Dest>::FlushImpl(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!ok())) return false;
  SyncBuffer(*dest_);
  bool flush_ok = true;
  if (flush_type != FlushType::kFromObject || dest_.is_owning()) {
    flush_ok = dest_->Flush(flush_type);
  }
  MakeBuffer(*dest_);
  return flush_ok;
}

}  // namespace riegeli

#endif  // RIEGELI_BYTES_POSITION_SHIFTING_WRITER_H_
