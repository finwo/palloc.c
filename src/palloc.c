// vim:fdm=marker:fdl=0

#ifdef __cplusplus
extern "C" {
#endif

#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "finwo/endian.h"

#if defined(_WIN32) || defined(_WIN64)
// Needs to be AFTER winsock2 which is used for endian.h
#include <windows.h>
#include <io.h>
#include <BaseTsd.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "finwo/canonical-path.h"

#include "palloc.h"

#define PALLOC_HTOBE_FLAGS(x)  htobe32(x)
#define PALLOC_HTOBE_SIZE(x)   htobe64(x)
#define PALLOC_HTOBE_OFFSET(x) htobe64(x)
#define PALLOC_BETOH_FLAGS(x)  be32toh(x)
#define PALLOC_BETOH_SIZE(x)   be64toh(x)
#define PALLOC_BETOH_OFFSET(x) be64toh(x)

/* struct palloc_t { */
/*   char     *filename; */
/*   int      descriptor; */
/*   uint32_t flags; */
/*   uint32_t header_size; */
/*   uint64_t first_free; */
/*   uint64_t size; */
/* }; */

#define PALLOC_MARKER_FREE (0x8000000000000000)

// OS-specific IO macros {{{
#if defined(_WIN32) || defined(_WIN64)
#define stat_os __stat64
#define fstat_os _fstat64
#define seek_os _lseeki64
#define open_os _open
#define write_os _write
#define read_os _read
#define close_os _close
#define O_CREAT _O_CREAT
#define O_RDWR  _O_RDWR
#define OPENMODE  (_S_IREAD | _S_IWRITE)
#define O_DSYNC 0
#define ssize_t SSIZE_T
#elif defined(__APPLE__)
#define stat_os stat
#define fstat_os fstat
#define seek_os lseek
#define open_os open
#define write_os write
#define read_os read
#define close_os close
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#else
#define stat_os stat64
#define fstat_os fstat64
#define seek_os lseek64
#define open_os open
#define write_os write
#define read_os read
#define close_os close
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#endif
// }}}

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

const char *expected_header      = "PBA\0";
#define     expected_header_size   4

struct palloc_fd_info {
  void *next;
  PALLOC_FD     fd;
  PALLOC_FLAGS  flags;
  PALLOC_OFFSET first_free;
  PALLOC_SIZE   header_size;
  PALLOC_SIZE   medium_size;
};

struct palloc_fd_info *_fd_info = NULL;

PALLOC_SIZE _palloc_marker(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  if (!ptr) return 0;
  PALLOC_SIZE result;
  seek_os(fd, ptr, SEEK_SET);
  read_os(fd, &result, sizeof(PALLOC_SIZE));
  result = PALLOC_BETOH_SIZE(result);
  return result;
}

PALLOC_SIZE _palloc_size(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  return _palloc_marker(fd, ptr) & (~PALLOC_MARKER_FREE);
}

struct palloc_fd_info * _palloc_info(PALLOC_FD fd) {
  PALLOC_OFFSET pos;
  PALLOC_SIZE   marker;

  // Attempt to fetch cached version
  struct palloc_fd_info *finfo = _fd_info;
  while(finfo) {
    if (finfo->fd == fd) break;
    finfo = finfo->next;
  }

  // Build new if no cached version was found
  if (!finfo) {
    finfo       = calloc(1, sizeof(struct palloc_fd_info));
    finfo->next = _fd_info;
    finfo->fd   = fd;
    _fd_info    = finfo;

    // Get the current medium size
    finfo->medium_size = seek_os(fd, 0, SEEK_END);

    // Detect header size
    finfo->header_size = expected_header_size + sizeof(PALLOC_FLAGS);
    char *hdr       = malloc(expected_header_size);
    seek_os(fd, 0, SEEK_SET);
    read_os(fd, hdr   , expected_header_size);
    read_os(fd, &(finfo->flags), sizeof(PALLOC_FLAGS));
    finfo->flags = PALLOC_BETOH_FLAGS(finfo->flags);
    if (finfo->flags & PALLOC_EXTENDED) {
      // Reserved for future use
      // header_size = bigger
    }
    free(hdr);

    // Detect first_free block
    pos = seek_os(fd, finfo->header_size, SEEK_SET);
    while(pos < finfo->medium_size) {
      if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) {
        perror("palloc_info::read");
        exit(1);
      }
      marker = PALLOC_BETOH_SIZE(marker);
      if (marker & PALLOC_MARKER_FREE) {
        break;
      }
      pos = seek_os(fd, marker + sizeof(marker), SEEK_CUR);
    }
    if (pos == finfo->medium_size) {
      finfo->first_free = 0;
    } else {
      finfo->first_free = pos;
    }
  }

  return finfo;
}
// }}}

PALLOC_FD palloc_open(const char *filename, PALLOC_FLAGS flags) {

  // Try to fetch the canonical path to the filename
  char *filepath = canonical_path(filename);
  if (!filepath) {
    perror("palloc_open::realpath");
    return 0;
  }

  // Prep real open flags
  int openFlags = O_RDWR | O_CREAT;
#if defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
  // No O_LARGEFILE needed
#else
  openFlags |= O_LARGEFILE;
#endif
  if (flags & PALLOC_SYNC) {
    openFlags |= O_DSYNC;
  }

  // Open the file
  PALLOC_FD fd = open_os(filepath, openFlags, OPENMODE);
  if (!fd) {
    perror("palloc_open::open");
    free(filepath);
    return 0;
  }

  // Pre-cache info
  _palloc_info(fd);

  free(filepath);
  return fd;
}

PALLOC_RESPONSE palloc_close(PALLOC_FD fd) {

  // Free fd info if we have it
  struct palloc_fd_info *finfo_cur = _fd_info;
  struct palloc_fd_info *finfo_prv = NULL;

  while(finfo_cur) {
    if (finfo_cur->fd == fd) break;
    finfo_prv = finfo_cur;
    finfo_cur = finfo_cur->next;
  }

  if (finfo_cur) {
    // Point prev to our next
    if (finfo_prv) finfo_prv->next = finfo_cur->next;
    else _fd_info = finfo_cur->next;
    // Free the info
    free(finfo_cur);
  }

  const int r = close_os(fd);
  if (r) {
    perror("close");
    return PALLOC_ERR;
  }
  return PALLOC_OK;
}

PALLOC_RESPONSE palloc_init(PALLOC_FD fd, PALLOC_FLAGS flags) {
  const int min_header_size = expected_header_size + sizeof(PALLOC_FLAGS);
  const int min_medium_size = min_header_size + (sizeof(PALLOC_SIZE)*2) + (sizeof(PALLOC_OFFSET)*2);
  char *z = calloc(min_medium_size, 1);

  // Fetch the file's size
  struct stat_os fst;
  if (fstat_os(fd, &fst)) {
    perror("palloc_init::fstat");
    free(z);
    return PALLOC_ERR;
  }
  int size = fst.st_size;

  // Make sure the medium has room for the header
  if (size < min_header_size) {
    if (flags & PALLOC_DYNAMIC) {
      seek_os(fd, 0, SEEK_SET);
      write_os(fd, z, min_header_size);
      size = min_header_size;
      seek_os(fd, 0, SEEK_SET);
    } else {
      fprintf(stderr, "Incompatible medium\n");
      free(z);
      return PALLOC_ERR;
    }
  }

  // Fix broken size
  if ((size > min_header_size) && (size < min_medium_size)) {
    if (flags & PALLOC_DYNAMIC) {
      seek_os(fd, min_header_size, SEEK_SET);
      write_os(fd, z, min_medium_size - min_header_size);
      size = min_medium_size;
      seek_os(fd, 0, SEEK_SET);
    } else {
      fprintf(stderr, "Incompatible medium\n");
      free(z);
      return PALLOC_ERR;
    }
  }

  // Actually read the header
  char *hdr  = malloc(min_header_size);
  seek_os(fd, 0, SEEK_SET);
  if (read_os(fd, hdr, min_header_size) != min_header_size) {
    perror("palloc_init::read");
    free(z);
    free(hdr);
    return PALLOC_ERR;
  }

  // Bail early if already initialized
  if (memcmp(hdr, expected_header, expected_header_size) == 0) {
    free(z);
    free(hdr);
    return PALLOC_OK;
  }

  // Build & write new header
  PALLOC_FLAGS nflags = PALLOC_HTOBE_FLAGS(flags & (~PALLOC_SYNC));
  memcpy(hdr, expected_header, expected_header_size);
  memcpy(hdr + expected_header_size, &nflags, sizeof(PALLOC_FLAGS));
  seek_os(fd, 0, SEEK_SET);
  if (write_os(fd, hdr, min_header_size) != min_header_size) {
    perror("palloc_init::write_header");
    free(z);
    free(hdr);
    return PALLOC_ERR;
  }

  // Mark remainder of medium free
  if (size >= min_medium_size) {
    PALLOC_SIZE   marker = PALLOC_HTOBE_SIZE((PALLOC_SIZE)((size - min_header_size - (sizeof(PALLOC_SIZE)*2)) | PALLOC_MARKER_FREE));
    PALLOC_OFFSET ptr    = PALLOC_HTOBE_OFFSET((PALLOC_OFFSET)0);
    seek_os(fd, min_header_size, SEEK_SET);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc_init::write_marker_start");
      free(z);
      free(hdr);
      return PALLOC_ERR;
    }
    if (write_os(fd, &ptr, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc_init::write_ptr_prev");
      free(z);
      free(hdr);
      return PALLOC_ERR;
    }
    if (write_os(fd, &ptr, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc_init::write_ptr_next");
      free(z);
      free(hdr);
      return PALLOC_ERR;
    }
    seek_os(fd, 0 - sizeof(PALLOC_SIZE), SEEK_END);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc_init::write_marker_end");
      free(z);
      free(hdr);
      return PALLOC_ERR;
    }
  }

  free(z);
  free(hdr);
  return PALLOC_OK;
}

PALLOC_OFFSET palloc(PALLOC_FD fd, PALLOC_SIZE size) {
  struct palloc_fd_info *finfo = _palloc_info(fd);

  PALLOC_SIZE marker, selected_size;
  PALLOC_OFFSET free_prev = 0, free_pprev = 0;
  PALLOC_OFFSET free_next = 0, free_nnext = 0;

  // Handle minimum size
  if (size < (sizeof(PALLOC_OFFSET)*2)) {
    size = sizeof(PALLOC_OFFSET) * 2;
  }

  // Iterate free blocks to find one that'll fit
  PALLOC_OFFSET selected = finfo->first_free;
  while(selected && (_palloc_size(fd, selected) < size)) {
    free_prev = selected;
    seek_os(fd, selected + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    read_os(fd, &selected, sizeof(PALLOC_OFFSET));
    selected = PALLOC_BETOH_OFFSET(selected);
  }

  // Handle full(-ish) medium when not dynamic
  if ((!selected) && (!(finfo->flags & PALLOC_DYNAMIC))) {
    return 0;
  }

  // Allocate new space if dynamic and needed
  if (!selected) {
    marker    = PALLOC_HTOBE_SIZE(size | PALLOC_MARKER_FREE);
    free_prev = PALLOC_HTOBE_OFFSET(free_prev);
    selected  = seek_os(fd, 0, SEEK_END);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    if (write_os(fd, &free_prev, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc::write");
      return 0;
    }
    free_prev = PALLOC_BETOH_OFFSET(free_prev);
    seek_os(fd, size - sizeof(PALLOC_OFFSET), SEEK_CUR);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    finfo->first_free  = selected;
    finfo->medium_size = seek_os(fd, 0, SEEK_CUR);
  }

  // Fetch selected block info
  marker = _palloc_marker(fd, selected);
  selected_size = _palloc_size(fd, selected);

  // Split block if large enough
  // marker,free_next,free_prev & fd position are dirty after this
  if ((selected_size - size) > ((sizeof(PALLOC_SIZE)*2)+(sizeof(PALLOC_OFFSET)*2))) {
    marker    = PALLOC_HTOBE_SIZE(size | PALLOC_MARKER_FREE);
    free_next = PALLOC_HTOBE_OFFSET(selected + size + (sizeof(PALLOC_SIZE)*2));
    free_prev = PALLOC_HTOBE_OFFSET(selected);
    // Update selected block
    seek_os(fd, selected, SEEK_SET);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    seek_os(fd, sizeof(PALLOC_OFFSET), SEEK_CUR);
    read_os(fd, &free_nnext, sizeof(PALLOC_OFFSET));
    seek_os(fd, 0 - sizeof(PALLOC_OFFSET), SEEK_CUR);
    if (write_os(fd, &free_next, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc::write");
      return 0;
    }
    seek_os(fd, size - (sizeof(PALLOC_OFFSET)*2), SEEK_CUR);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    // Initialize new free block
    marker = PALLOC_HTOBE_SIZE((selected_size - size - (sizeof(PALLOC_SIZE)*2)) | PALLOC_MARKER_FREE);
    free_pprev = PALLOC_HTOBE_OFFSET(seek_os(fd, 0, SEEK_CUR));
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    if (write_os(fd, &free_prev, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc::write");
      return 0;
    }
    if (write_os(fd, &free_nnext, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc::write");
      return 0;
    }
    seek_os(fd, (PALLOC_BETOH_SIZE(marker) & (~PALLOC_MARKER_FREE)) - (sizeof(PALLOC_OFFSET)*2), SEEK_CUR);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    // Update next block's pointer
    free_nnext = PALLOC_BETOH_OFFSET(free_nnext);
    if (free_nnext) {
      seek_os(fd, free_nnext + sizeof(PALLOC_SIZE), SEEK_SET);
      if (write_os(fd, &free_pprev, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
        perror("palloc::write");
        return 0;
      }
    }
  }

  // Size is now block size, not given size
  size = _palloc_size(fd, selected);

  // Remove selected free block from the doubly-linked-list
  seek_os(fd, selected + sizeof(PALLOC_SIZE), SEEK_SET);
  read_os(fd, &free_prev, sizeof(PALLOC_OFFSET));
  read_os(fd, &free_next, sizeof(PALLOC_OFFSET));
  free_prev = PALLOC_BETOH_OFFSET(free_prev);
  free_next = PALLOC_BETOH_OFFSET(free_next);
  if (free_prev) {
    free_next = PALLOC_HTOBE_OFFSET(free_next);
    seek_os(fd, free_prev + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    write_os(fd, &free_next, sizeof(PALLOC_OFFSET));
    free_next = PALLOC_BETOH_OFFSET(free_next);
  }
  if (free_next) {
    free_prev = PALLOC_HTOBE_OFFSET(free_prev);
    seek_os(fd, free_next + sizeof(PALLOC_SIZE), SEEK_SET);
    write_os(fd, &free_prev, sizeof(PALLOC_OFFSET));
    free_prev = PALLOC_BETOH_OFFSET(free_prev);
  }

  // Fix first free
  if (finfo->first_free == selected) {
    finfo->first_free = free_next;
  }

  // Mark selected block as non-free
  marker = PALLOC_HTOBE_SIZE(size);
  seek_os(fd, selected, SEEK_SET);
  if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
    perror("palloc::write");
    return 0;
  }
  seek_os(fd, size, SEEK_CUR);
  if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
    perror("palloc::write");
    return 0;
  }

  // And return the pointer to the start of the data
  return selected + sizeof(PALLOC_SIZE);
}

void _pfree_merge(PALLOC_FD fd, PALLOC_OFFSET left, PALLOC_OFFSET right) {
  PALLOC_SIZE left_marker  = _palloc_marker(fd, left );
  PALLOC_SIZE right_marker = _palloc_marker(fd, right);
  PALLOC_SIZE left_size    = left_marker  & (~PALLOC_MARKER_FREE);
  PALLOC_SIZE right_size   = right_marker & (~PALLOC_MARKER_FREE);
  PALLOC_OFFSET right_next;

  // Not both free = do not merge
  if (!(left_marker & right_marker & PALLOC_MARKER_FREE)) {
    return;
  }

  // Not consecutive = do not merge
  if ((left + left_size + (sizeof(PALLOC_SIZE)*2)) != right) {
    return;
  }

  // Read the right_next, as that'll become our next
  seek_os(fd, right + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
  read_os(fd, &right_next, sizeof(PALLOC_OFFSET));

  // Actually merge the blocks into 1 big one
  left_size   = left_size + right_size + (sizeof(PALLOC_SIZE)*2);
  left_marker = PALLOC_HTOBE_SIZE(left_size | PALLOC_MARKER_FREE);
  seek_os(fd, left, SEEK_SET);
  write_os(fd, &left_marker, sizeof(left_marker));
  seek_os(fd, sizeof(PALLOC_OFFSET), SEEK_CUR);
  write_os(fd, &right_next, sizeof(right_next));
  seek_os(fd, left_size - (sizeof(PALLOC_OFFSET)*2), SEEK_CUR);
  write_os(fd, &left_marker, sizeof(left_marker));

  // Update right_next's prev pointer
  if (PALLOC_BETOH_OFFSET(right_next)) {
    left = PALLOC_HTOBE_OFFSET(left);
    seek_os(fd, PALLOC_BETOH_OFFSET(right_next) + sizeof(PALLOC_SIZE), SEEK_SET);
    write_os(fd, &left, sizeof(left));
    left = PALLOC_BETOH_OFFSET(left);
  }
}

PALLOC_RESPONSE pfree(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  PALLOC_SIZE marker, size;
  PALLOC_OFFSET off_left, off_right;

  // Convert pointer to outer
  ptr -= sizeof(PALLOC_SIZE);

  // Fetch info
  struct palloc_fd_info *finfo = _palloc_info(fd);

  // Get the pointer's own marker in advance
  // Bail early if already free
  seek_os(fd, ptr, SEEK_SET);
  read_os(fd, &marker, sizeof(PALLOC_SIZE));
  marker = PALLOC_BETOH_SIZE(marker);
  if (marker & PALLOC_MARKER_FREE) {
    return PALLOC_OK;
  }

  // Detect free neighbours
  PALLOC_OFFSET free_prev = 0;
  PALLOC_OFFSET free_next = 0;
  PALLOC_OFFSET free_cur  = finfo->first_free;
  while(free_cur) {
    if (free_cur < ptr)   free_prev = free_cur;
    if (free_cur > ptr) { free_next = free_cur; break; }
    seek_os(fd, free_cur + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    read_os(fd, &free_cur, sizeof(PALLOC_OFFSET));
    free_cur = PALLOC_BETOH_OFFSET(free_cur);
  }

  // We need BE pointers during the next block
  free_prev = PALLOC_HTOBE_OFFSET(free_prev);
  free_next = PALLOC_HTOBE_OFFSET(free_next);

  // Mark ourselves as free & link into free list
  seek_os(fd, ptr, SEEK_SET);
  read_os(fd, &marker, sizeof(PALLOC_SIZE));
  size   = PALLOC_BETOH_SIZE(marker) & (~PALLOC_MARKER_FREE);
  marker = PALLOC_HTOBE_SIZE(size | PALLOC_MARKER_FREE);
  seek_os(fd, ptr, SEEK_SET);
  write_os(fd, &marker, sizeof(marker));
  write_os(fd, &free_prev, sizeof(free_prev));
  write_os(fd, &free_next, sizeof(free_next));
  seek_os(fd, size - (sizeof(PALLOC_SIZE)*2), SEEK_CUR);
  write_os(fd, &marker, sizeof(marker));

  // Update first_free if needed
  if ((!(finfo->first_free)) || (finfo->first_free > ptr)) {
    finfo->first_free = ptr;
  }

  // We need H pointers during the next block
  free_prev = PALLOC_BETOH_OFFSET(free_prev);
  free_next = PALLOC_BETOH_OFFSET(free_next);

  // Update our neighbours' pointers
  if (free_prev) {
    seek_os(fd, free_prev + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    off_left = PALLOC_HTOBE_OFFSET(ptr);
    write_os(fd, &off_left, sizeof(PALLOC_SIZE));
  }
  if (free_next) {
    seek_os(fd, free_next + sizeof(PALLOC_SIZE), SEEK_SET);
    off_right = PALLOC_HTOBE_OFFSET(ptr);
    write_os(fd, &off_right, sizeof(PALLOC_SIZE));
  }

  // Merge with neighbours if consecutive
  // Next first, so we don't need to update our tracking
  if (free_next) _pfree_merge(fd, ptr, free_next);
  if (free_prev) _pfree_merge(fd, free_prev, ptr);

  // TODO: if dynamic and we're last in the file, truncate
  return PALLOC_OK;
}

PALLOC_SIZE palloc_size(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  return _palloc_size(fd, ptr - sizeof(PALLOC_SIZE));
}

PALLOC_OFFSET palloc_next(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  struct palloc_fd_info *finfo = _palloc_info(fd);
  PALLOC_SIZE marker;

  // Easy resolve
  if (ptr >= finfo->medium_size) return 0;

  // Handle first
  if (!ptr) {
    ptr = finfo->header_size;
    seek_os(fd, ptr, SEEK_SET);
    if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) return 0;
    marker = PALLOC_BETOH_SIZE(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return ptr + sizeof(marker);

  // Convert pointer to internal usage
  } else {
    ptr -= sizeof(PALLOC_SIZE);
  }

  // Read the marker of the given block
  seek_os(fd, ptr, SEEK_SET);
  if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) return 0;
  marker = PALLOC_BETOH_SIZE(marker);

  // Skip the first one
  ptr = ptr + (sizeof(PALLOC_SIZE) * 2) + (marker & (~PALLOC_MARKER_FREE));
  while(1) {
    if (ptr >= finfo->medium_size) return 0;
    seek_os(fd, ptr, SEEK_SET);
    if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) return 0;
    marker = PALLOC_BETOH_SIZE(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return ptr + sizeof(marker);
    ptr += (sizeof(marker)*2) + (marker & (~PALLOC_MARKER_FREE));
  }

  return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
