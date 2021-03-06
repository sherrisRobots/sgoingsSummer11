/*
 *  tcmalloc.cc
 *  Avida
 *
 *  Added by David on 10/14/09.
 *  Copyright 2009-2010 Michigan State University. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// Copyright (c) 2005, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>
//
// A malloc that uses a per-thread cache to satisfy small malloc requests.
// (The time for malloc/free of a small object drops from 300 ns to 50 ns.)
//
// See doc/tcmalloc.html for a high-level
// description of how this malloc works.
//
// SYNCHRONIZATION
//  1. The thread-specific lists are accessed without acquiring any locks.
//     This is safe because each such list is only accessed by one thread.
//  2. We have a lock per central free-list, and hold it while manipulating
//     the central free list for a particular size.
//  3. The central page allocator is protected by "pageheap_lock".
//  4. The pagemap (which maps from page-number to descriptor),
//     can be read without holding any locks, and written while holding
//     the "pageheap_lock".
//  5. To improve performance, a subset of the information one can get
//     from the pagemap is cached in a data structure, pagemap_cache_,
//     that atomically reads and writes its entries.  This cache can be
//     read and written without locking.
//
//     This multi-threaded access to the pagemap is safe for fairly
//     subtle reasons.  We basically assume that when an object X is
//     allocated by thread A and deallocated by thread B, there must
//     have been appropriate synchronization in the handoff of object
//     X from thread A to thread B.  The same logic applies to pagemap_cache_.
//
// THE PAGEID-TO-SIZECLASS CACHE
// Hot PageID-to-sizeclass mappings are held by pagemap_cache_.  If this cache
// returns 0 for a particular PageID then that means "no information," not that
// the sizeclass is 0.  The cache may have stale information for pages that do
// not hold the beginning of any free()'able object.  Staleness is eliminated
// in Populate() for pages with sizeclass > 0 objects, and in do_malloc() and
// do_memalign() for all other relevant pages.
//
// PAGEMAP
// -------
// Page map contains a mapping from page id to Span.
//
// If Span s occupies pages [p..q],
//      pagemap[p] == s
//      pagemap[q] == s
//      pagemap[p+1..q-1] are undefined
//      pagemap[p-1] and pagemap[q+1] are defined:
//         NULL if the corresponding page is not yet in the address space.
//         Otherwise it points to a Span.  This span may be free
//         or allocated.  If free, it is in one of pageheap's freelist.
//
// TODO: Bias reclamation to larger addresses
// TODO: implement mallinfo/mallopt
// TODO: Better testing
//
// 9/28/2003 (new page-level allocator replaces ptmalloc2):
// * malloc/free of small objects goes from ~300 ns to ~50 ns.
// * allocation of a reasonably complicated struct
//   goes from about 1100 ns to about 300 ns.

#include "tcmalloc-platform.h"
#include <new>
#include <stdio.h>
#include <stddef.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <string.h>
#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <algorithm>

#include "tcmalloc.h"
#include "commandlineflags.h"
#include "basictypes.h"               // gets us PRIu64
#include "sysinfo.h"
#include "spinlock.h"
#include "common.h"
#include "malloc_extension.h"
#include "central_freelist.h"
#include "internal_logging.h"
#include "linked_list.h"
#include "page_heap.h"
#include "page_heap_allocator.h"
#include "pagemap.h"
#include "span.h"
#include "static_vars.h"
#include "system-alloc.h"
#include "tcmalloc_guard.h"
#include "thread_cache.h"

#if (defined(_WIN32) && !defined(__CYGWIN__) && !defined(__CYGWIN32__)) && !defined(WIN32_OVERRIDE_ALLOCATORS)
# define WIN32_DO_PATCHING 1
#endif

using tcmalloc::PageHeap;
using tcmalloc::PageHeapAllocator;
using tcmalloc::SizeMap;
using tcmalloc::Span;
using tcmalloc::Static;
using tcmalloc::ThreadCache;

// __THROW is defined in glibc systems.  It means, counter-intuitively,
// "This function will never throw an exception."  It's an optional
// optimization tool, but we may need to use it to match glibc prototypes.
#ifndef __THROW    // I guess we're not on a glibc system
# define __THROW   // __THROW is just an optimization, so ok to make it ""
#endif

DECLARE_int64(tcmalloc_sample_parameter);
DECLARE_double(tcmalloc_release_rate);


// Override the libc functions to prefer our own instead.  This comes
// first so code in tcmalloc.cc can use the overridden versions.  One
// exception: in windows, by default, we patch our code into these
// functions (via src/windows/patch_function.cc) rather than override
// them.  In that case, we don't want to do this overriding here.
#if !defined(WIN32_DO_PATCHING) && !defined(TCMALLOC_FOR_DEBUGALLOCATION)

#if defined(__GNUC__) && !defined(__MACH__)
  // Potentially faster variants that use the gcc alias extension.
  // FreeBSD does support aliases, but apparently not correctly. :-(
  // NOTE: we make many of these symbols weak, but do so in the makefile
  //       (via objcopy -W) and not here.  That ends up being more portable.
# define ALIAS(x) __attribute__ ((alias (x)))
void* operator new(size_t size)                  ALIAS("tc_new");
void operator delete(void* p) __THROW            ALIAS("tc_delete");
void* operator new[](size_t size)                ALIAS("tc_newarray");
void operator delete[](void* p) __THROW          ALIAS("tc_deletearray");
void* operator new(size_t size, const std::nothrow_t&) __THROW
                                                 ALIAS("tc_new_nothrow");
void* operator new[](size_t size, const std::nothrow_t&) __THROW
                                                 ALIAS("tc_newarray_nothrow");
extern "C" {
  void* malloc(size_t size) __THROW              ALIAS("tc_malloc");
  void  free(void* ptr) __THROW                  ALIAS("tc_free");
  void* realloc(void* ptr, size_t size) __THROW  ALIAS("tc_realloc");
  void* calloc(size_t n, size_t size) __THROW    ALIAS("tc_calloc");
  void  cfree(void* ptr) __THROW                 ALIAS("tc_cfree");
  void* memalign(size_t align, size_t s) __THROW ALIAS("tc_memalign");
  void* valloc(size_t size) __THROW              ALIAS("tc_valloc");
  void* pvalloc(size_t size) __THROW             ALIAS("tc_pvalloc");
  int posix_memalign(void** r, size_t a, size_t s) __THROW
      ALIAS("tc_posix_memalign");
  void malloc_stats(void) __THROW                ALIAS("tc_malloc_stats");
  int mallopt(int cmd, int value) __THROW        ALIAS("tc_mallopt");
#ifdef HAVE_STRUCT_MALLINFO
  struct mallinfo mallinfo(void) __THROW         ALIAS("tc_mallinfo");
#endif
}   // extern "C"
#else  // #if defined(__GNUC__) && !defined(__MACH__)
// Portable wrappers
void* operator new(size_t size)                  { return tc_new(size);       }
void operator delete(void* p) __THROW            { tc_delete(p);              }
void* operator new[](size_t size)                { return tc_newarray(size);  }
void operator delete[](void* p) __THROW          { tc_deletearray(p);         }
void* operator new(size_t size, const std::nothrow_t& nt) __THROW {
  return tc_new_nothrow(size, nt);
}
void* operator new[](size_t size, const std::nothrow_t& nt) __THROW {
  return tc_newarray_nothrow(size, nt);
}
extern "C" {
  void* malloc(size_t s) __THROW                 { return tc_malloc(s);       }
  void  free(void* p) __THROW                    { tc_free(p);                }
  void* realloc(void* p, size_t s) __THROW       { return tc_realloc(p, s);   }
  void* calloc(size_t n, size_t s) __THROW       { return tc_calloc(n, s);    }
  void  cfree(void* p) __THROW                   { tc_cfree(p);               }
  void* memalign(size_t a, size_t s) __THROW     { return tc_memalign(a, s);  }
  void* valloc(size_t s) __THROW                 { return tc_valloc(s);       }
  void* pvalloc(size_t s) __THROW                { return tc_pvalloc(s);      }
  int posix_memalign(void** r, size_t a, size_t s) __THROW {
    return tc_posix_memalign(r, a, s);
  }
  void malloc_stats(void) __THROW                { tc_malloc_stats();         }
  int mallopt(int cmd, int v) __THROW            { return tc_mallopt(cmd, v); }
#ifdef HAVE_STRUCT_MALLINFO
  struct mallinfo mallinfo(void) __THROW         { return tc_mallinfo();      }
#endif
}  // extern "C"
#endif  // #if defined(__GNUC__)

// Some library routines on RedHat 9 allocate memory using malloc()
// and free it using __libc_free() (or vice-versa).  Since we provide
// our own implementations of malloc/free, we need to make sure that
// the __libc_XXX variants (defined as part of glibc) also point to
// the same implementations.
#ifdef __GLIBC__       // only glibc defines __libc_*
extern "C" {
#ifdef ALIAS
  void* __libc_malloc(size_t size)               ALIAS("tc_malloc");
  void  __libc_free(void* ptr)                   ALIAS("tc_free");
  void* __libc_realloc(void* ptr, size_t size)   ALIAS("tc_realloc");
  void* __libc_calloc(size_t n, size_t size)     ALIAS("tc_calloc");
  void  __libc_cfree(void* ptr)                  ALIAS("tc_cfree");
  void* __libc_memalign(size_t align, size_t s)  ALIAS("tc_memalign");
  void* __libc_valloc(size_t size)               ALIAS("tc_valloc");
  void* __libc_pvalloc(size_t size)              ALIAS("tc_pvalloc");
  int __posix_memalign(void** r, size_t a, size_t s) ALIAS("tc_posix_memalign");
#else  // #ifdef ALIAS
  void* __libc_malloc(size_t size)               { return malloc(size);       }
  void  __libc_free(void* ptr)                   { free(ptr);                 }
  void* __libc_realloc(void* ptr, size_t size)   { return realloc(ptr, size); }
  void* __libc_calloc(size_t n, size_t size)     { return calloc(n, size);    }
  void  __libc_cfree(void* ptr)                  { cfree(ptr);                }
  void* __libc_memalign(size_t align, size_t s)  { return memalign(align, s); }
  void* __libc_valloc(size_t size)               { return valloc(size);       }
  void* __libc_pvalloc(size_t size)              { return pvalloc(size);      }
  int __posix_memalign(void** r, size_t a, size_t s) {
    return posix_memalign(r, a, s);
  }
#endif  // #ifdef ALIAS
}   // extern "C"
#endif  // ifdef __GLIBC__

#undef ALIAS

#endif  // #ifndef(WIN32_DO_PATCHING) && ndef(TCMALLOC_FOR_DEBUGALLOCATION)


// ----------------------- IMPLEMENTATION -------------------------------

// Routines such as free() and realloc() catch some erroneous pointers
// passed to them, and invoke the below when they do.  (An erroneous pointer
// won't be caught if it's within a valid span or a stale span for which
// the pagemap cache has a non-zero sizeclass.) This is a cheap (source-editing
// required) kind of exception handling for these routines.
namespace {
void InvalidFree(void* ptr) {
  CRASH("Attempt to free invalid pointer: %p\n", ptr);
}

size_t InvalidGetSizeForRealloc(void* old_ptr) {
  CRASH("Attempt to realloc invalid pointer: %p\n", old_ptr);
  return 0;
}

size_t InvalidGetAllocatedSize(void* ptr) {
  CRASH("Attempt to get the size of an invalid pointer: %p\n", ptr);
  return 0;
}
}  // unnamed namespace

// Extract interesting stats
struct TCMallocStats {
  uint64_t system_bytes;        // Bytes alloced from system
  uint64_t thread_bytes;        // Bytes in thread caches
  uint64_t central_bytes;       // Bytes in central cache
  uint64_t transfer_bytes;      // Bytes in central transfer cache
  uint64_t pageheap_bytes;      // Bytes in page heap
  uint64_t metadata_bytes;      // Bytes alloced for metadata
};

// Get stats into "r".  Also get per-size-class counts if class_count != NULL
static void ExtractStats(TCMallocStats* r, uint64_t* class_count) {
  r->central_bytes = 0;
  r->transfer_bytes = 0;
  for (unsigned int cl = 0; cl < kNumClasses; ++cl) {
    const int length = Static::central_cache()[cl].length();
    const int tc_length = Static::central_cache()[cl].tc_length();
    const size_t size = static_cast<uint64_t>(
        Static::sizemap()->ByteSizeForClass(cl));
    r->central_bytes += (size * length);
    r->transfer_bytes += (size * tc_length);
    if (class_count) class_count[cl] = length + tc_length;
  }

  // Add stats from per-thread heaps
  r->thread_bytes = 0;
  { // scope
    SpinLockHolder h(Static::pageheap_lock());
    ThreadCache::GetThreadStats(&r->thread_bytes, class_count);
  }

  { //scope
    SpinLockHolder h(Static::pageheap_lock());
    r->system_bytes = Static::pageheap()->SystemBytes();
    r->metadata_bytes = tcmalloc::metadata_system_bytes();
    r->pageheap_bytes = Static::pageheap()->FreeBytes();
  }
}

// WRITE stats to "out"
static void DumpStats(TCMalloc_Printer* out, int level) {
  TCMallocStats stats;
  uint64_t class_count[kNumClasses];
  ExtractStats(&stats, (level >= 2 ? class_count : NULL));

  static const double MB = 1048576.0;

  if (level >= 2) {
    out->printf("------------------------------------------------\n");
    out->printf("Size class breakdown\n");
    out->printf("------------------------------------------------\n");
    uint64_t cumulative = 0;
    for (unsigned int cl = 0; cl < kNumClasses; ++cl) {
      if (class_count[cl] > 0) {
        uint64_t class_bytes =
            class_count[cl] * Static::sizemap()->ByteSizeForClass(cl);
        cumulative += class_bytes;
        out->printf("class %3d [ %8" PRIuS " bytes ] : "
                "%8" PRIu64 " objs; %5.1f MB; %5.1f cum MB\n",
                cl, Static::sizemap()->ByteSizeForClass(cl),
                class_count[cl],
                class_bytes / MB,
                cumulative / MB);
      }
    }

    SpinLockHolder h(Static::pageheap_lock());
    Static::pageheap()->Dump(out);

    out->printf("------------------------------------------------\n");
    DumpSystemAllocatorStats(out);
  }

  const uint64_t bytes_in_use = stats.system_bytes
                                - stats.pageheap_bytes
                                - stats.central_bytes
                                - stats.transfer_bytes
                                - stats.thread_bytes;

  out->printf("------------------------------------------------\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Heap size\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Bytes in use by application\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Bytes free in page heap\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Bytes free in central cache\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Bytes free in transfer cache\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Bytes free in thread caches\n"
              "MALLOC: %12" PRIu64 "              Spans in use\n"
              "MALLOC: %12" PRIu64 "              Thread heaps in use\n"
              "MALLOC: %12" PRIu64 " (%7.1f MB) Metadata allocated\n"
              "------------------------------------------------\n",
              stats.system_bytes, stats.system_bytes / MB,
              bytes_in_use, bytes_in_use / MB,
              stats.pageheap_bytes, stats.pageheap_bytes / MB,
              stats.central_bytes, stats.central_bytes / MB,
              stats.transfer_bytes, stats.transfer_bytes / MB,
              stats.thread_bytes, stats.thread_bytes / MB,
              uint64_t(Static::span_allocator()->inuse()),
              uint64_t(ThreadCache::HeapsInUse()),
              stats.metadata_bytes, stats.metadata_bytes / MB);
}

static void PrintStats(int level) {
  const int kBufferSize = 16 << 10;
  char* buffer = new char[kBufferSize];
  TCMalloc_Printer printer(buffer, kBufferSize);
  DumpStats(&printer, level);
  write(STDERR_FILENO, buffer, strlen(buffer));
  delete[] buffer;
}

// TCMalloc's support for extra malloc interfaces
class TCMallocImplementation : public MallocExtension {
 public:
  virtual void GetStats(char* buffer, int buffer_length) {
    ASSERT(buffer_length > 0);
    TCMalloc_Printer printer(buffer, buffer_length);

    // Print level one stats unless lots of space is available
    if (buffer_length < 10000) {
      DumpStats(&printer, 1);
    } else {
      DumpStats(&printer, 2);
    }
  }

  virtual bool GetNumericProperty(const char* name, size_t* value) {
    ASSERT(name != NULL);

    if (strcmp(name, "generic.current_allocated_bytes") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.system_bytes
               - stats.thread_bytes
               - stats.central_bytes
               - stats.transfer_bytes
               - stats.pageheap_bytes;
      return true;
    }

    if (strcmp(name, "generic.heap_size") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.system_bytes;
      return true;
    }

    if (strcmp(name, "tcmalloc.slack_bytes") == 0) {
      // We assume that bytes in the page heap are not fragmented too
      // badly, and are therefore available for allocation.
      SpinLockHolder l(Static::pageheap_lock());
      *value = Static::pageheap()->FreeBytes();
      return true;
    }

    if (strcmp(name, "tcmalloc.max_total_thread_cache_bytes") == 0) {
      SpinLockHolder l(Static::pageheap_lock());
      *value = ThreadCache::overall_thread_cache_size();
      return true;
    }

    if (strcmp(name, "tcmalloc.current_total_thread_cache_bytes") == 0) {
      TCMallocStats stats;
      ExtractStats(&stats, NULL);
      *value = stats.thread_bytes;
      return true;
    }

    return false;
  }

  virtual bool SetNumericProperty(const char* name, size_t value) {
    ASSERT(name != NULL);

    if (strcmp(name, "tcmalloc.max_total_thread_cache_bytes") == 0) {
      SpinLockHolder l(Static::pageheap_lock());
      ThreadCache::set_overall_thread_cache_size(value);
      return true;
    }

    return false;
  }

  virtual void MarkThreadIdle() {
    ThreadCache::BecomeIdle();
  }

  virtual void MarkThreadBusy();  // Implemented below

  virtual void ReleaseFreeMemory() {
    SpinLockHolder h(Static::pageheap_lock());
    Static::pageheap()->ReleaseFreePages();
  }

  virtual void SetMemoryReleaseRate(double rate) {
    FLAGS_tcmalloc_release_rate = rate;
  }

  virtual double GetMemoryReleaseRate() {
    return FLAGS_tcmalloc_release_rate;
  }
  virtual size_t GetEstimatedAllocatedSize(size_t size) {
    if (size <= kMaxSize) {
      const size_t cl = Static::sizemap()->SizeClass(size);
      const size_t alloc_size = Static::sizemap()->ByteSizeForClass(cl);
      return alloc_size;
    } else {
      return tcmalloc::pages(size) << kPageShift;
    }
  }

  // This just calls GetSizeWithCallback, but because that's in an
  // unnamed namespace, we need to move the definition below it in the
  // file.
  virtual size_t GetAllocatedSize(void* ptr);
};

// The constructor allocates an object to ensure that initialization
// runs before main(), and therefore we do not have a chance to become
// multi-threaded before initialization.  We also create the TSD key
// here.  Presumably by the time this constructor runs, glibc is in
// good enough shape to handle pthread_key_create().
//
// The constructor also takes the opportunity to tell STL to use
// tcmalloc.  We want to do this early, before construct time, so
// all user STL allocations go through tcmalloc (which works really
// well for STL).
//
// The destructor prints stats when the program exits.
static int tcmallocguard_refcount = 0;  // no lock needed: runs before main()
TCMallocGuard::TCMallocGuard() {
  if (tcmallocguard_refcount++ == 0) {
#ifdef HAVE_TLS    // this is true if the cc/ld/libc combo support TLS
    // Check whether the kernel also supports TLS (needs to happen at runtime)
    tcmalloc::CheckIfKernelSupportsTLS();
#endif
#ifdef WIN32_DO_PATCHING
    // patch the windows VirtualAlloc, etc.
    PatchWindowsFunctions();    // defined in windows/patch_functions.cc
#endif
    free(malloc(1));
    ThreadCache::InitTSD();
    free(malloc(1));
    MallocExtension::Register(new TCMallocImplementation);
  }
}

TCMallocGuard::~TCMallocGuard() {
  if (--tcmallocguard_refcount == 0) {
    const char* env = getenv("MALLOCSTATS");
    if (env != NULL) {
      int level = atoi(env);
      if (level < 1) level = 1;
      PrintStats(level);
    }
  }
}
#ifndef WIN32_OVERRIDE_ALLOCATORS
static TCMallocGuard module_enter_exit_hook;
#endif

//-------------------------------------------------------------------
// Helpers for the exported routines below
//-------------------------------------------------------------------

static inline bool CheckCachedSizeClass(void *ptr) {
  PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  size_t cached_value = Static::pageheap()->GetSizeClassIfCached(p);
  return cached_value == 0 ||
      cached_value == Static::pageheap()->GetDescriptor(p)->sizeclass;
}

static inline void* CheckedMallocResult(void *result)
{
  ASSERT(result == 0 || CheckCachedSizeClass(result));
  return result;
}

static inline void* SpanToMallocResult(Span *span) {
  Static::pageheap()->CacheSizeClass(span->start, 0);
  return
      CheckedMallocResult(reinterpret_cast<void*>(span->start << kPageShift));
}


namespace {

// Helper for do_malloc().
inline void* do_malloc_pages(Length num_pages) {
  Span *span;
  {
    SpinLockHolder h(Static::pageheap_lock());
    span = Static::pageheap()->New(num_pages);
  }

  return (span == NULL ? NULL : SpanToMallocResult(span));
}

inline void* do_malloc(size_t size) {
  void* ret = NULL;

  // The following call forces module initialization
  ThreadCache* heap = ThreadCache::GetCache();
  if (size <= kMaxSize) {
    // The common case, and also the simplest.  This just pops the
    // size-appropriate freelist, after replenishing it if it's empty.
    ret = CheckedMallocResult(heap->Allocate(size));
  } else {
    ret = do_malloc_pages(tcmalloc::pages(size));
  }
  if (ret == NULL) errno = ENOMEM;
  return ret;
}

inline void* do_calloc(size_t n, size_t elem_size) {
  // Overflow check
  const size_t size = n * elem_size;
  if (elem_size != 0 && size / elem_size != n) return NULL;

  void* result = do_malloc(size);
  if (result != NULL) {
    memset(result, 0, size);
  }
  return result;
}

static inline ThreadCache* GetCacheIfPresent() {
  void* const p = ThreadCache::GetCacheIfPresent();
  return reinterpret_cast<ThreadCache*>(p);
}

// This lets you call back to a given function pointer if ptr is invalid.
// It is used primarily by windows code which wants a specialized callback.
inline void do_free_with_callback(void* ptr, void (*invalid_free_fn)(void*)) {
  if (ptr == NULL) return;
  ASSERT(Static::pageheap() != NULL);  // Should not call free() before malloc()
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  Span* span = NULL;
  size_t cl = Static::pageheap()->GetSizeClassIfCached(p);

  if (cl == 0) {
    span = Static::pageheap()->GetDescriptor(p);
    if (!span) {
      // span can be NULL because the pointer passed in is invalid
      // (not something returned by malloc or friends), or because the
      // pointer was allocated with some other allocator besides
      // tcmalloc.  The latter can happen if tcmalloc is linked in via
      // a dynamic library, but is not listed last on the link line.
      // In that case, libraries after it on the link line will
      // allocate with libc malloc, but free with tcmalloc's free.
      (*invalid_free_fn)(ptr);  // Decide how to handle the bad free request
      return;
    }
    cl = span->sizeclass;
    Static::pageheap()->CacheSizeClass(p, cl);
  }
  if (cl != 0) {
    ThreadCache* heap = GetCacheIfPresent();
    if (heap != NULL) {
      heap->Deallocate(ptr, cl);
    } else {
      // Delete directly into central cache
      tcmalloc::SLL_SetNext(ptr, NULL);
      Static::central_cache()[cl].InsertRange(ptr, ptr, 1);
    }
  } else {
    SpinLockHolder h(Static::pageheap_lock());
    ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
    ASSERT(span != NULL && span->start == p);
    Static::pageheap()->Delete(span);
  }
}

// The default "do_free" that uses the default callback.
inline void do_free(void* ptr) {
  return do_free_with_callback(ptr, &InvalidFree);
}

inline size_t GetSizeWithCallback(void* ptr,
                                  size_t (*invalid_getsize_fn)(void*)) {
  if (ptr == NULL)
    return 0;
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  size_t cl = Static::pageheap()->GetSizeClassIfCached(p);
  if (cl != 0) {
    return Static::sizemap()->ByteSizeForClass(cl);
  } else {
    Span *span = Static::pageheap()->GetDescriptor(p);
    if (span == NULL) {  // means we do not own this memory
      return (*invalid_getsize_fn)(ptr);
    } else if (span->sizeclass != 0) {
      Static::pageheap()->CacheSizeClass(p, span->sizeclass);
      return Static::sizemap()->ByteSizeForClass(span->sizeclass);
    } else {
      return span->length << kPageShift;
    }
  }
}

// This lets you call back to a given function pointer if ptr is invalid.
// It is used primarily by windows code which wants a specialized callback.
inline void* do_realloc_with_callback(
    void* old_ptr, size_t new_size,
    void (*invalid_free_fn)(void*),
    size_t (*invalid_get_size_fn)(void*)) {
  // Get the size of the old entry
  const size_t old_size = GetSizeWithCallback(old_ptr, invalid_get_size_fn);

  // Reallocate if the new size is larger than the old size,
  // or if the new size is significantly smaller than the old size.
  // We do hysteresis to avoid resizing ping-pongs:
  //    . If we need to grow, grow to max(new_size, old_size * 1.X)
  //    . Don't shrink unless new_size < old_size * 0.Y
  // X and Y trade-off time for wasted space.  For now we do 1.25 and 0.5.
  const unsigned int lower_bound_to_grow = old_size + old_size / 4;
  const unsigned int upper_bound_to_shrink = old_size / 2;
  if ((new_size > old_size) || (new_size < upper_bound_to_shrink)) {
    // Need to reallocate.
    void* new_ptr = NULL;

    if (new_size > old_size && new_size < lower_bound_to_grow) {
      new_ptr = do_malloc(lower_bound_to_grow);
    }
    if (new_ptr == NULL) {
      // Either new_size is not a tiny increment, or last do_malloc failed.
      new_ptr = do_malloc(new_size);
    }
    if (new_ptr == NULL) {
      return NULL;
    }
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
    // We could use a variant of do_free() that leverages the fact
    // that we already know the sizeclass of old_ptr.  The benefit
    // would be small, so don't bother.
    do_free_with_callback(old_ptr, invalid_free_fn);
    return new_ptr;
  } else {
    return old_ptr;
  }
}

inline void* do_realloc(void* old_ptr, size_t new_size) {
  return do_realloc_with_callback(old_ptr, new_size,
                                  &InvalidFree, &InvalidGetSizeForRealloc);
}

// For use by exported routines below that want specific alignments
//
// Note: this code can be slow, and can significantly fragment memory.
// The expectation is that memalign/posix_memalign/valloc/pvalloc will
// not be invoked very often.  This requirement simplifies our
// implementation and allows us to tune for expected allocation
// patterns.
void* do_memalign(size_t align, size_t size) {
  ASSERT((align & (align - 1)) == 0);
  ASSERT(align > 0);
  if (size + align < size) return NULL;         // Overflow

  if (Static::pageheap() == NULL) ThreadCache::InitModule();

  // Allocate at least one byte to avoid boundary conditions below
  if (size == 0) size = 1;

  if (size <= kMaxSize && align < kPageSize) {
    // Search through acceptable size classes looking for one with
    // enough alignment.  This depends on the fact that
    // InitSizeClasses() currently produces several size classes that
    // are aligned at powers of two.  We will waste time and space if
    // we miss in the size class array, but that is deemed acceptable
    // since memalign() should be used rarely.
    unsigned int cl = Static::sizemap()->SizeClass(size);
    while (cl < kNumClasses &&
           ((Static::sizemap()->class_to_size(cl) & (align - 1)) != 0)) {
      cl++;
    }
    if (cl < kNumClasses) {
      ThreadCache* heap = ThreadCache::GetCache();
      return CheckedMallocResult(heap->Allocate(
                                     Static::sizemap()->class_to_size(cl)));
    }
  }

  // We will allocate directly from the page heap
  SpinLockHolder h(Static::pageheap_lock());

  if (align <= kPageSize) {
    // Any page-level allocation will be fine
    // TODO: We could put the rest of this page in the appropriate
    // TODO: cache but it does not seem worth it.
    Span* span = Static::pageheap()->New(tcmalloc::pages(size));
    return span == NULL ? NULL : SpanToMallocResult(span);
  }

  // Allocate extra pages and carve off an aligned portion
  const Length alloc = tcmalloc::pages(size + align);
  Span* span = Static::pageheap()->New(alloc);
  if (span == NULL) return NULL;

  // Skip starting portion so that we end up aligned
  Length skip = 0;
  while ((((span->start+skip) << kPageShift) & (align - 1)) != 0) {
    skip++;
  }
  ASSERT(skip < alloc);
  if (skip > 0) {
    Span* rest = Static::pageheap()->Split(span, skip);
    Static::pageheap()->Delete(span);
    span = rest;
  }

  // Skip trailing portion that we do not need to return
  const Length needed = tcmalloc::pages(size);
  ASSERT(span->length >= needed);
  if (span->length > needed) {
    Span* trailer = Static::pageheap()->Split(span, needed);
    Static::pageheap()->Delete(trailer);
  }
  return SpanToMallocResult(span);
}

// Helpers for use by exported routines below:

inline void do_malloc_stats() {
  PrintStats(1);
}

inline int do_mallopt(int cmd, int value) {
  return 1;     // Indicates error
}

#ifdef HAVE_STRUCT_MALLINFO  // mallinfo isn't defined on freebsd, for instance
inline struct mallinfo do_mallinfo() {
  TCMallocStats stats;
  ExtractStats(&stats, NULL);

  // Just some of the fields are filled in.
  struct mallinfo info;
  memset(&info, 0, sizeof(info));

  // Unfortunately, the struct contains "int" field, so some of the
  // size values will be truncated.
  info.arena     = static_cast<int>(stats.system_bytes);
  info.fsmblks   = static_cast<int>(stats.thread_bytes
                                    + stats.central_bytes
                                    + stats.transfer_bytes);
  info.fordblks  = static_cast<int>(stats.pageheap_bytes);
  info.uordblks  = static_cast<int>(stats.system_bytes
                                    - stats.thread_bytes
                                    - stats.central_bytes
                                    - stats.transfer_bytes
                                    - stats.pageheap_bytes);

  return info;
}
#endif  // #ifndef HAVE_STRUCT_MALLINFO

static SpinLock set_new_handler_lock(SpinLock::LINKER_INITIALIZED);

inline void* cpp_alloc(size_t size, bool nothrow) {
  for (;;) {
    void* p = do_malloc(size);
#ifdef PREANSINEW
    return p;
#else
    if (p == NULL) {  // allocation failed
      // Get the current new handler.  NB: this function is not
      // thread-safe.  We make a feeble stab at making it so here, but
      // this lock only protects against tcmalloc interfering with
      // itself, not with other libraries calling set_new_handler.
      std::new_handler nh;
      {
        SpinLockHolder h(&set_new_handler_lock);
        nh = std::set_new_handler(0);
        (void) std::set_new_handler(nh);
      }
#if (defined(__GNUC__) && !defined(__EXCEPTIONS)) || (defined(_HAS_EXCEPTIONS) && !_HAS_EXCEPTIONS)
      if (nh) {
        // Since exceptions are disabled, we don't really know if new_handler
        // failed.  Assume it will abort if it fails.
        (*nh)();
        continue;
      }
      return 0;
#else
      // If no new_handler is established, the allocation failed.
      if (!nh) {
        if (nothrow) return 0;
        throw std::bad_alloc();
      }
      // Otherwise, try the new_handler.  If it returns, retry the
      // allocation.  If it throws std::bad_alloc, fail the allocation.
      // if it throws something else, don't interfere.
      try {
        (*nh)();
      } catch (const std::bad_alloc&) {
        if (!nothrow) throw;
        return p;
      }
#endif  // (defined(__GNUC__) && !defined(__EXCEPTIONS)) || (defined(_HAS_EXCEPTIONS) && !_HAS_EXCEPTIONS)
    } else {  // allocation success
      return p;
    }
#endif  // PREANSINEW
  }
}

}  // end unnamed namespace

// As promised, the definition of this function, declared above.
size_t TCMallocImplementation::GetAllocatedSize(void* ptr) {
  return GetSizeWithCallback(ptr, &InvalidGetAllocatedSize);
}

void TCMallocImplementation::MarkThreadBusy() {
  // Allocate to force the creation of a thread cache, but avoid
  // invoking any hooks.
  do_free(do_malloc(0));
}

//-------------------------------------------------------------------
// Exported routines
//-------------------------------------------------------------------

extern "C" PERFTOOLS_DLL_DECL const char* tc_version(
    int* major, int* minor, const char** patch) __THROW {
  if (major) *major = TC_VERSION_MAJOR;
  if (minor) *minor = TC_VERSION_MINOR;
  if (patch) *patch = TC_VERSION_PATCH;
  return TC_VERSION_STRING;
}

static int tc_new_mode = 0;  // See tc_set_new_mode().
extern "C" PERFTOOLS_DLL_DECL void* tc_malloc(size_t size) __THROW {
  return (tc_new_mode ? cpp_alloc(size, false) : do_malloc(size));
}

extern "C" PERFTOOLS_DLL_DECL void tc_free(void* ptr) __THROW {
  do_free(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_calloc(size_t n, size_t elem_size) __THROW {
  return do_calloc(n, elem_size);
}

extern "C" PERFTOOLS_DLL_DECL void tc_cfree(void* ptr) __THROW {
  do_free(ptr);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_realloc(void* old_ptr,
                                               size_t new_size) __THROW {
  if (old_ptr == NULL) {
    void* result = do_malloc(new_size);
    return result;
  }
  if (new_size == 0) {
    do_free(old_ptr);
    return NULL;
  }
  return do_realloc(old_ptr, new_size);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new(size_t size) {
  return cpp_alloc(size, false);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_new_nothrow(size_t size, const std::nothrow_t&) __THROW {
  return cpp_alloc(size, true);
}

extern "C" PERFTOOLS_DLL_DECL void tc_delete(void* p) __THROW {
  do_free(p);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray(size_t size) {
  return cpp_alloc(size, false);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_newarray_nothrow(size_t size, const std::nothrow_t&) __THROW {
  return cpp_alloc(size, true);
}

extern "C" PERFTOOLS_DLL_DECL void tc_deletearray(void* p) __THROW {
  do_free(p);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_memalign(size_t align, size_t size) __THROW {
  return do_memalign(align, size);
}

extern "C" PERFTOOLS_DLL_DECL int tc_posix_memalign(void** result_ptr, size_t align, size_t size) __THROW {
  if (((align % sizeof(void*)) != 0) ||
      ((align & (align - 1)) != 0) ||
      (align == 0)) {
    return EINVAL;
  }

  void* result = do_memalign(align, size);
  if (result == NULL) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

static size_t pagesize = 0;

extern "C" PERFTOOLS_DLL_DECL void* tc_valloc(size_t size) __THROW {
  // Allocate page-aligned object of length >= size bytes
  if (pagesize == 0) pagesize = getpagesize();
  return do_memalign(pagesize, size);
}

extern "C" PERFTOOLS_DLL_DECL void* tc_pvalloc(size_t size) __THROW {
  // Round up size to a multiple of pagesize
  if (pagesize == 0) pagesize = getpagesize();
  if (size == 0) {     // pvalloc(0) should allocate one page, according to
    size = pagesize;   // http://man.free4web.biz/man3/libmpatrol.3.html
  }
  size = (size + pagesize - 1) & ~(pagesize - 1);
  return do_memalign(pagesize, size);
}

extern "C" PERFTOOLS_DLL_DECL void tc_malloc_stats(void) __THROW {
  do_malloc_stats();
}

extern "C" PERFTOOLS_DLL_DECL int tc_mallopt(int cmd, int value) __THROW {
  return do_mallopt(cmd, value);
}

#ifdef HAVE_STRUCT_MALLINFO
extern "C" PERFTOOLS_DLL_DECL struct mallinfo tc_mallinfo(void) __THROW {
  return do_mallinfo();
}
#endif

// This function behaves similarly to MSVC's _set_new_mode.
// If flag is 0 (default), calls to malloc will behave normally.
// If flag is 1, calls to malloc will behave like calls to new,
// and the std_new_handler will be invoked on failure.
// Returns the previous mode.
extern "C" PERFTOOLS_DLL_DECL int tc_set_new_mode(int flag) __THROW {
  int old_mode = tc_new_mode;
  tc_new_mode = flag;
  return old_mode;
}


// Override __libc_memalign in libc on linux boxes specially.
// They have a bug in libc that causes them to (very rarely) allocate
// with __libc_memalign() yet deallocate with free() and the
// definitions above don't catch it.
// This function is an exception to the rule of calling MallocHook method
// from the stack frame of the allocation function;
// heap-checker handles this special case explicitly.
#ifndef TCMALLOC_FOR_DEBUGALLOCATION
static void *MemalignOverride(size_t align, size_t size, const void *caller) __THROW ATTRIBUTE_SECTION(google_malloc);

static void *MemalignOverride(size_t align, size_t size, const void *caller) __THROW {
  return do_memalign(align, size);
}
void *(*__memalign_hook)(size_t, size_t, const void *) = MemalignOverride;
#endif  // #ifndef TCMALLOC_FOR_DEBUGALLOCATION
