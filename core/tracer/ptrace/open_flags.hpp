#pragma once
// Pure open(2)-flag classification for the default filters.
// Unit-tested in test/unit/test_open_flags.cpp; used by the ptrace backend.
// O_TMPFILE implies O_DIRECTORY at the bit level, so it must be tested first.
#ifdef __linux__
#include <fcntl.h>
#endif

namespace snowglobe::tracer {

struct OpenFlags {
  bool write = false;
  bool create = false;
  bool trunc = false;
  bool tmpfile = false;
  // O_DIRECTORY or O_PATH without O_TMPFILE: skipped by default (-a restores).
  bool dirOrPath = false;
};

inline OpenFlags classifyOpenFlags(unsigned long flags, bool isCreat) {
  OpenFlags of;
  if (isCreat) {
    // creat(2) has no flags argument: O_WRONLY|O_CREAT|O_TRUNC.
    of.write = true;
    of.create = true;
    of.trunc = true;
    return of;
  }
#ifdef __linux__
#ifdef O_TMPFILE
  of.tmpfile = (flags & O_TMPFILE) == O_TMPFILE;
#endif
  if (of.tmpfile) {
    // O_TMPFILE requires write access; the opened object has no name, so the
    // event carries the directory path plus tmpfile:true.
    of.write = true;
    return of;
  }
  bool dirPath = false;
#ifdef O_DIRECTORY
  dirPath = dirPath || ((flags & O_DIRECTORY) != 0);
#endif
#ifdef O_PATH
  dirPath = dirPath || ((flags & O_PATH) != 0);
#endif
  of.dirOrPath = dirPath;
  if (dirPath) {
    return of;
  }
  of.write = ((flags & O_WRONLY) != 0) || ((flags & O_RDWR) != 0);
  of.create = (flags & O_CREAT) != 0;
  of.trunc = (flags & O_TRUNC) != 0;
#else
  (void)flags;
#endif
  return of;
}

} // namespace snowglobe::tracer
