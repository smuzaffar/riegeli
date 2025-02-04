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

#ifndef _WIN32

// Make `pread()` and `posix_fadvise()` available.
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 600
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

// Make `off_t` 64-bit even on 32-bit systems.
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

// Make `copy_file_range()` available on Linux.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#else

#define WIN32_LEAN_AND_MEAN

#endif

#include "riegeli/bytes/fd_reader.h"

#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <cerrno>
#include <limits>
#include <memory>
#include <string>
#ifndef _WIN32
#include <type_traits>
#endif
#include <utility>

#include "absl/base/optimization.h"
#ifndef _WIN32
#include "absl/meta/type_traits.h"
#endif
#include "absl/status/status.h"
#ifndef _WIN32
#include "absl/strings/match.h"
#endif
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "riegeli/base/arithmetic.h"
#include "riegeli/base/assert.h"
#ifdef _WIN32
#include "riegeli/base/errno_mapping.h"
#endif
#include "riegeli/base/no_destructor.h"
#include "riegeli/base/object.h"
#include "riegeli/base/status.h"
#include "riegeli/base/types.h"
#ifdef _WIN32
#include "riegeli/base/unicode.h"
#endif
#include "riegeli/bytes/buffered_reader.h"
#include "riegeli/bytes/fd_internal.h"
#ifndef _WIN32
#include "riegeli/bytes/fd_writer.h"
#endif
#include "riegeli/bytes/reader.h"
#ifndef _WIN32
#include "riegeli/bytes/writer.h"
#endif

namespace riegeli {

#ifndef _WIN32

namespace {

// `copy_file_range()` is supported by Linux and FreeBSD.

template <typename FirstArg, typename Enable = void>
struct HaveCopyFileRange : std::false_type {};

template <typename FirstArg>
struct HaveCopyFileRange<
    FirstArg,
    absl::void_t<decltype(copy_file_range(
        std::declval<FirstArg>(), std::declval<fd_internal::Offset*>(),
        std::declval<int>(), std::declval<fd_internal::Offset*>(),
        std::declval<size_t>(), std::declval<unsigned>()))>> : std::true_type {
};

template <typename FirstArg,
          std::enable_if_t<HaveCopyFileRange<FirstArg>::value, int> = 0>
inline ssize_t CopyFileRange(FirstArg src, fd_internal::Offset* src_offset,
                             int dest, fd_internal::Offset* dest_offset,
                             size_t length, unsigned flags) {
  return copy_file_range(src, src_offset, dest, dest_offset, length, flags);
}

template <typename FirstArg,
          std::enable_if_t<!HaveCopyFileRange<FirstArg>::value, int> = 0>
inline ssize_t CopyFileRange(FirstArg src, fd_internal::Offset* src_offset,
                             int dest, fd_internal::Offset* dest_offset,
                             size_t length, unsigned flags) {
  errno = EOPNOTSUPP;
  return -1;
}

// `posix_fadvise()` is supported by POSIX systems but not MacOS.

template <typename FirstArg, typename Enable = void>
struct HavePosixFadvise : std::false_type {};

template <typename FirstArg>
struct HavePosixFadvise<
    FirstArg, absl::void_t<decltype(posix_fadvise(
                  std::declval<FirstArg>(), std::declval<fd_internal::Offset>(),
                  std::declval<fd_internal::Offset>(), std::declval<int>()))>>
    : std::true_type {};

template <typename FirstArg,
          std::enable_if_t<HavePosixFadvise<FirstArg>::value, int> = 0>
inline void FdSetReadAllHint(FirstArg src, bool read_all_hint) {
#ifdef POSIX_FADV_SEQUENTIAL
  posix_fadvise(src, 0, 0,
                read_all_hint ? POSIX_FADV_SEQUENTIAL : POSIX_FADV_NORMAL);
#endif
}

template <typename FirstArg,
          std::enable_if_t<!HavePosixFadvise<FirstArg>::value, int> = 0>
inline void FdSetReadAllHint(FirstArg src, bool read_all_hint) {}

}  // namespace

#endif

void FdReaderBase::Initialize(int src,
#ifdef _WIN32
                              int mode,
#endif
                              absl::optional<std::string>&& assumed_filename,
                              absl::optional<Position> assumed_pos,
                              absl::optional<Position> independent_pos) {
  RIEGELI_ASSERT_GE(src, 0)
      << "Failed precondition of FdReader: negative file descriptor";
  filename_ = fd_internal::ResolveFilename(src, std::move(assumed_filename));
  InitializePos(src,
#ifdef _WIN32
                mode, /*mode_was_passed_to_open=*/false,
#endif
                assumed_pos, independent_pos);
}

int FdReaderBase::OpenFd(absl::string_view filename, int mode) {
#ifndef _WIN32
  RIEGELI_ASSERT((mode & O_ACCMODE) == O_RDONLY || (mode & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdReader: "
         "mode must include either O_RDONLY or O_RDWR";
#else
  RIEGELI_ASSERT((mode & (_O_RDONLY | _O_WRONLY | _O_RDWR)) == _O_RDONLY ||
                 (mode & (_O_RDONLY | _O_WRONLY | _O_RDWR)) == _O_RDWR)
      << "Failed precondition of FdReader: "
         "mode must include either _O_RDONLY or _O_RDWR";
#endif
  // TODO: When `absl::string_view` becomes C++17 `std::string_view`:
  // `filename_ = filename`
  filename_.assign(filename.data(), filename.size());
#ifndef _WIN32
again:
  const int src = open(filename_.c_str(), mode, 0666);
  if (ABSL_PREDICT_FALSE(src < 0)) {
    if (errno == EINTR) goto again;
    BufferedReader::Reset(kClosed);
    FailOperation("open()");
    return -1;
  }
#else
  std::wstring filename_wide;
  if (ABSL_PREDICT_FALSE(!Utf8ToWide(filename_, filename_wide))) {
    BufferedReader::Reset(kClosed);
    Fail(absl::InvalidArgumentError("Filename not valid UTF-8"));
    return -1;
  }
  int src;
  if (ABSL_PREDICT_FALSE(_wsopen_s(&src, filename_wide.c_str(), mode,
                                   _SH_DENYNO, _S_IREAD) != 0)) {
    BufferedReader::Reset(kClosed);
    FailOperation("_wsopen_s()");
    return -1;
  }
#endif
  return src;
}

void FdReaderBase::InitializePos(int src,
#ifdef _WIN32
                                 int mode, bool mode_was_passed_to_open,
#endif
                                 absl::optional<Position> assumed_pos,
                                 absl::optional<Position> independent_pos) {
  RIEGELI_ASSERT(!has_independent_pos_)
      << "Failed precondition of FdReaderBase::InitializePos(): "
         "has_independent_pos_ not reset";
  RIEGELI_ASSERT(!supports_random_access_)
      << "Failed precondition of FdReaderBase::InitializePos(): "
         "supports_random_access_ not reset";
  RIEGELI_ASSERT_EQ(random_access_status_, absl::OkStatus())
      << "Failed precondition of FdReaderBase::InitializePos(): "
         "random_access_status_ not reset";
#ifdef _WIN32
  RIEGELI_ASSERT(original_mode_ == absl::nullopt)
      << "Failed precondition of FdWriterBase::InitializePos(): "
         "original_mode_ not reset";
  int text_mode =
      mode & (_O_BINARY | _O_TEXT | _O_WTEXT | _O_U16TEXT | _O_U8TEXT);
  if (!mode_was_passed_to_open && text_mode != 0) {
    const int original_mode = _setmode(src, text_mode);
    if (ABSL_PREDICT_FALSE(original_mode < 0)) {
      FailOperation("_setmode()");
      return;
    }
    original_mode_ = original_mode;
  }
  if (assumed_pos == absl::nullopt) {
    if (text_mode == 0) {
      // There is no `_getmode()`, but `_setmode()` returns the previous mode.
      text_mode = _setmode(src, _O_BINARY);
      if (ABSL_PREDICT_FALSE(text_mode < 0)) {
        FailOperation("_setmode()");
        return;
      }
      if (ABSL_PREDICT_FALSE(_setmode(src, text_mode) < 0)) {
        FailOperation("_setmode()");
        return;
      }
    }
    if (text_mode != _O_BINARY) {
      if (ABSL_PREDICT_FALSE(independent_pos != absl::nullopt)) {
        Fail(absl::InvalidArgumentError(
            "FdReaderBase::Options::independent_pos() requires binary mode"));
        return;
      }
      assumed_pos = 0;
    }
  }
#endif
  if (assumed_pos != absl::nullopt) {
    if (ABSL_PREDICT_FALSE(independent_pos != absl::nullopt)) {
      Fail(absl::InvalidArgumentError(
          "FdReaderBase::Options::assumed_pos() and independent_pos() "
          "must not be both set"));
      return;
    }
    if (ABSL_PREDICT_FALSE(
            *assumed_pos >
            Position{std::numeric_limits<fd_internal::Offset>::max()})) {
      FailOverflow();
      return;
    }
    set_limit_pos(*assumed_pos);
    // `supports_random_access_` is left as `false`.
    static const NoDestructor<absl::Status> status(absl::UnimplementedError(
        "FdReaderBase::Options::assumed_pos() excludes random access"));
    random_access_status_ = *status;
  } else if (independent_pos != absl::nullopt) {
    has_independent_pos_ = true;
    if (ABSL_PREDICT_FALSE(
            *independent_pos >
            Position{std::numeric_limits<fd_internal::Offset>::max()})) {
      FailOverflow();
      return;
    }
    set_limit_pos(*independent_pos);
    supports_random_access_ = true;
  } else {
    const fd_internal::Offset file_pos = fd_internal::LSeek(src, 0, SEEK_CUR);
    if (file_pos < 0) {
      // Random access is not supported. Assume 0 as the initial position.
      // `supports_random_access_` is left as `false`.
      random_access_status_ =
          FailedOperationStatus(fd_internal::kLSeekFunctionName);
      return;
    }
    set_limit_pos(IntCast<Position>(file_pos));

    // Check if random access is supported.
#ifndef _WIN32
    if (ABSL_PREDICT_FALSE(absl::StartsWith(filename(), "/sys/"))) {
      // "/sys" files do not support random access. It is hard to reliably
      // recognize them, so `FdReader` checks the filename.
      //
      // Some "/proc" files also do not support random access, but they are
      // recognized by a failing `fd_internal::LSeek(SEEK_END)`.
      //
      // `supports_random_access_` is left as `false`.
      random_access_status_ =
          absl::UnimplementedError("/sys files do not support random access");
    } else
#endif
    {
      const fd_internal::Offset file_size =
          fd_internal::LSeek(src, 0, SEEK_END);
      if (file_size < 0) {
        // Not supported. `supports_random_access_` is left as `false`.
        random_access_status_ =
            FailedOperationStatus(fd_internal::kLSeekFunctionName);
      } else {
        // Supported.
        supports_random_access_ = true;
        if (ABSL_PREDICT_FALSE(
                fd_internal::LSeek(src,
                                   IntCast<fd_internal::Offset>(limit_pos()),
                                   SEEK_SET) < 0)) {
          FailOperation(fd_internal::kLSeekFunctionName);
          return;
        }
        if (!growing_source_) set_exact_size(IntCast<Position>(file_size));
      }
    }
  }
  BeginRun();
}

void FdReaderBase::Done() {
  BufferedReader::Done();
#ifdef _WIN32
  if (original_mode_ != absl::nullopt) {
    const int src = SrcFd();
    if (ABSL_PREDICT_FALSE(_setmode(src, *original_mode_) < 0)) {
      FailOperation("_setmode()");
    }
  }
#endif
  random_access_status_ = absl::OkStatus();
}

inline absl::Status FdReaderBase::FailedOperationStatus(
    absl::string_view operation) {
  const int error_number = errno;
  RIEGELI_ASSERT_NE(error_number, 0)
      << "Failed precondition of FdReaderBase::FailedOperationStatus(): "
         "zero errno";
  return absl::ErrnoToStatus(error_number, absl::StrCat(operation, " failed"));
}

bool FdReaderBase::FailOperation(absl::string_view operation) {
  return Fail(FailedOperationStatus(operation));
}

#ifdef _WIN32

bool FdReaderBase::FailWindowsOperation(absl::string_view operation) {
  const DWORD error_number = GetLastError();
  RIEGELI_ASSERT_NE(error_number, 0)
      << "Failed precondition of FdReaderBase::FailWindowsOperation(): "
         "zero error code";
  return Fail(WindowsErrorToStatus(IntCast<uint32_t>(error_number),
                                   absl::StrCat(operation, " failed")));
}

#endif

absl::Status FdReaderBase::AnnotateStatusImpl(absl::Status status) {
  if (!filename_.empty()) {
    status = Annotate(status, absl::StrCat("reading ", filename_));
  }
  return BufferedReader::AnnotateStatusImpl(std::move(status));
}

#ifndef _WIN32

void FdReaderBase::SetReadAllHintImpl(bool read_all_hint) {
  BufferedReader::SetReadAllHintImpl(read_all_hint);
  if (ABSL_PREDICT_FALSE(!ok())) return;
  const int src = SrcFd();
  FdSetReadAllHint(src, read_all_hint);
}

#endif

bool FdReaderBase::ReadInternal(size_t min_length, size_t max_length,
                                char* dest) {
  RIEGELI_ASSERT_GT(min_length, 0u)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "nothing to read";
  RIEGELI_ASSERT_GE(max_length, min_length)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "max_length < min_length";
  RIEGELI_ASSERT(ok())
      << "Failed precondition of BufferedReader::ReadInternal(): " << status();
  const int src = SrcFd();
  for (;;) {
    if (ABSL_PREDICT_FALSE(
            limit_pos() >=
            Position{std::numeric_limits<fd_internal::Offset>::max()})) {
      return FailOverflow();
    }
#ifndef _WIN32
    const size_t length_to_read = UnsignedMin(
        max_length,
        Position{std::numeric_limits<fd_internal::Offset>::max()} - limit_pos(),
        size_t{std::numeric_limits<ssize_t>::max()},
        // Darwin and FreeBSD cannot read more than 2 GB - 1 at a time.
        // Limit to 1 GB for better alignment of reads.
        // https://codereview.appspot.com/89900044#msg9
        size_t{1} << 30);
  again:
    const ssize_t length_read =
        has_independent_pos_ ? pread(src, dest, length_to_read,
                                     IntCast<fd_internal::Offset>(limit_pos()))
                             : read(src, dest, length_to_read);
    if (ABSL_PREDICT_FALSE(length_read < 0)) {
      if (errno == EINTR) goto again;
      return FailOperation(has_independent_pos_ ? "pread()" : "read()");
    }
#else
    DWORD length_read;
    if (has_independent_pos_) {
      const HANDLE file_handle = reinterpret_cast<HANDLE>(_get_osfhandle(src));
      if (ABSL_PREDICT_FALSE(file_handle == INVALID_HANDLE_VALUE ||
                             file_handle == reinterpret_cast<HANDLE>(-2))) {
        return FailWindowsOperation("_get_osfhandle()");
      }
      const DWORD length_to_read = IntCast<DWORD>(UnsignedMin(
          max_length,
          Position{std::numeric_limits<fd_internal::Offset>::max()} -
              limit_pos(),
          std::numeric_limits<DWORD>::max()));
      OVERLAPPED overlapped{};
      overlapped.Offset = IntCast<DWORD>(limit_pos() & 0xffffffff);
      overlapped.OffsetHigh = IntCast<DWORD>(limit_pos() >> 32);
      if (ABSL_PREDICT_FALSE(!ReadFile(file_handle, dest, length_to_read,
                                       &length_read, &overlapped)) &&
          ABSL_PREDICT_FALSE(GetLastError() != ERROR_HANDLE_EOF)) {
        return FailWindowsOperation("ReadFile()");
      }
    } else {
      const unsigned length_to_read = UnsignedMin(
          max_length,
          Position{std::numeric_limits<fd_internal::Offset>::max()} -
              limit_pos(),
          unsigned{std::numeric_limits<int>::max()});
      const int length_read_int = _read(src, dest, length_to_read);
      if (ABSL_PREDICT_FALSE(length_read_int < 0)) {
        return FailOperation("_read()");
      }
      length_read = IntCast<DWORD>(length_read_int);
    }
#endif
    if (ABSL_PREDICT_FALSE(length_read == 0)) {
      if (!growing_source_) set_exact_size(limit_pos());
      return false;
    }
    RIEGELI_ASSERT_LE(IntCast<size_t>(length_read), max_length)
#ifndef _WIN32
        << (has_independent_pos_ ? "pread()" : "read()")
#else
        << (has_independent_pos_ ? "ReadFile()" : "_read()")
#endif
        << " read more than requested";
    move_limit_pos(IntCast<size_t>(length_read));
    if (IntCast<size_t>(length_read) >= min_length) return true;
    dest += length_read;
    min_length -= IntCast<size_t>(length_read);
    max_length -= IntCast<size_t>(length_read);
  }
}

#ifndef _WIN32

bool FdReaderBase::CopyInternal(Position length, Writer& dest) {
  RIEGELI_ASSERT_GT(length, 0u)
      << "Failed precondition of BufferedReader::CopyInternal(): "
         "nothing to copy";
  RIEGELI_ASSERT(ok())
      << "Failed precondition of BufferedReader::CopyInternal(): " << status();
  if (HaveCopyFileRange<int>::value) {
    {
      FdWriterBase* const fd_writer = dest.GetIf<FdWriterBase>();
      if (fd_writer != nullptr) {
        const int src = SrcFd();
        for (;;) {
          if (ABSL_PREDICT_FALSE(!fd_writer->Flush(FlushType::kFromObject))) {
            return false;
          }
          const int dest_fd = fd_writer->DestFd();
          fd_internal::Offset src_offset = limit_pos();
          fd_internal::Offset dest_offset = fd_writer->start_pos();
          if (ABSL_PREDICT_FALSE(
                  limit_pos() >=
                  Position{std::numeric_limits<fd_internal::Offset>::max()})) {
            return FailOverflow();
          }
          const size_t length_to_copy = UnsignedMin(
              length,
              Position{std::numeric_limits<fd_internal::Offset>::max()} -
                  limit_pos(),
              size_t{std::numeric_limits<ssize_t>::max()});
          if (ABSL_PREDICT_FALSE(
                  length_to_copy >
                  Position{std::numeric_limits<fd_internal::Offset>::max()} -
                      fd_writer->start_pos())) {
            return fd_writer->FailOverflow();
          }
        again:
          const ssize_t length_copied = CopyFileRange(
              src, has_independent_pos_ ? &src_offset : nullptr, dest_fd,
              fd_writer->has_independent_pos_ ? &dest_offset : nullptr,
              length_to_copy, 0);
          if (ABSL_PREDICT_FALSE(length_copied < 0)) {
            if (errno == EINTR) goto again;
            // File descriptors might not support `copy_file_range()` for a
            // variety of reasons, e.g. append mode, not regular files,
            // unsupported filesystem, or cross filesystem copy. Fall back to
            // `read()` and `write()`.
            break;
          }
          if (ABSL_PREDICT_FALSE(length_copied == 0)) {
            if (!growing_source_) set_exact_size(limit_pos());
            return false;
          }
          RIEGELI_ASSERT_LE(IntCast<size_t>(length_copied), length_to_copy)
              << "copy_file_range() copied more than requested";
          move_limit_pos(IntCast<size_t>(length_copied));
          fd_writer->move_start_pos(IntCast<size_t>(length_copied));
          length -= IntCast<size_t>(length_copied);
          if (length == 0) return true;
        }
      }
    }
  }
  return BufferedReader::CopyInternal(length, dest);
}

#endif

inline bool FdReaderBase::SeekInternal(int src, Position new_pos) {
  RIEGELI_ASSERT_EQ(available(), 0u)
      << "Failed precondition of FdReaderBase::SeekInternal(): "
         "buffer not empty";
  RIEGELI_ASSERT(FdReaderBase::SupportsRandomAccess())
      << "Failed precondition of FdReaderBase::SeekInternal(): "
         "random access not supported";
  if (!has_independent_pos_) {
    if (ABSL_PREDICT_FALSE(
            fd_internal::LSeek(src, IntCast<fd_internal::Offset>(new_pos),
                               SEEK_SET) < 0)) {
      return FailOperation(fd_internal::kLSeekFunctionName);
    }
  }
  set_limit_pos(new_pos);
  return true;
}

bool FdReaderBase::SeekBehindBuffer(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos())
      << "Failed precondition of BufferedReader::SeekBehindBuffer(): "
         "position in the buffer, use Seek() instead";
  RIEGELI_ASSERT_EQ(start_to_limit(), 0u)
      << "Failed precondition of BufferedReader::SeekBehindBuffer(): "
         "buffer not empty";
  if (ABSL_PREDICT_FALSE(!FdReaderBase::SupportsRandomAccess())) {
    if (ABSL_PREDICT_FALSE(new_pos < start_pos())) {
      if (ok()) Fail(random_access_status_);
      return false;
    }
    return BufferedReader::SeekBehindBuffer(new_pos);
  }
  if (ABSL_PREDICT_FALSE(!ok())) return false;
  const int src = SrcFd();
  if (new_pos > limit_pos()) {
    // Seeking forwards.
    Position file_size;
    if (exact_size() != absl::nullopt) {
      file_size = *exact_size();
    } else {
      fd_internal::StatInfo stat_info;
      if (ABSL_PREDICT_FALSE(fd_internal::FStat(src, &stat_info) < 0)) {
        return FailOperation(fd_internal::kFStatFunctionName);
      }
      file_size = IntCast<Position>(stat_info.st_size);
      if (!growing_source_) set_exact_size(file_size);
    }
    if (ABSL_PREDICT_FALSE(new_pos > file_size)) {
      // File ends.
      SeekInternal(src, file_size);
      return false;
    }
  }
  return SeekInternal(src, new_pos);
}

absl::optional<Position> FdReaderBase::SizeImpl() {
  if (ABSL_PREDICT_FALSE(!ok())) return absl::nullopt;
  if (exact_size() != absl::nullopt) return *exact_size();
  if (ABSL_PREDICT_FALSE(!FdReaderBase::SupportsRandomAccess())) {
    Fail(random_access_status_);
    return absl::nullopt;
  }
  const int src = SrcFd();
  fd_internal::StatInfo stat_info;
  if (ABSL_PREDICT_FALSE(fd_internal::FStat(src, &stat_info) < 0)) {
    FailOperation(fd_internal::kFStatFunctionName);
    return absl::nullopt;
  }
  if (!growing_source_) set_exact_size(IntCast<Position>(stat_info.st_size));
  return IntCast<Position>(stat_info.st_size);
}

std::unique_ptr<Reader> FdReaderBase::NewReaderImpl(Position initial_pos) {
  if (ABSL_PREDICT_FALSE(!FdReaderBase::SupportsNewReader())) {
    if (ok()) {
      Fail(
#ifdef _WIN32
          !has_independent_pos_
              ? absl::UnimplementedError(
                    "FdReaderBase::Options::independent_pos() "
                    "required for read mode")
              :
#endif
              random_access_status_);
    }
    return nullptr;
  }
  if (ABSL_PREDICT_FALSE(!ok())) return nullptr;
  // `NewReaderImpl()` is thread-safe from this point.
  const int src = SrcFd();
  std::unique_ptr<FdReader<UnownedFd>> reader =
      std::make_unique<FdReader<UnownedFd>>(
          src, FdReaderBase::Options()
                   .set_assumed_filename(filename())
                   .set_independent_pos(initial_pos)
                   .set_growing_source(growing_source_)
                   .set_buffer_options(buffer_options()));
  reader->set_exact_size(exact_size());
  ShareBufferTo(*reader);
  return reader;
}

}  // namespace riegeli
