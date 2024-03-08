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
  result = be64toh(result);
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
    finfo->flags = be32toh(finfo->flags);
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
      marker = be64toh(marker);
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
  PALLOC_FLAGS nflags = htobe32(flags & (~PALLOC_SYNC));
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
    PALLOC_SIZE   marker = htobe64((PALLOC_SIZE)((size - min_header_size - (sizeof(PALLOC_SIZE)*2)) | PALLOC_MARKER_FREE));
    PALLOC_OFFSET ptr    = htobe64((PALLOC_OFFSET)0);
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
    selected = be64toh(selected);
  }

  // Handle full(-ish) medium when not dynamic
  if ((!selected) && (!(finfo->flags & PALLOC_DYNAMIC))) {
    return 0;
  }

  // Allocate new space if dynamic and needed
  if (!selected) {
    marker    = htobe64(size | PALLOC_MARKER_FREE);
    free_prev = htobe64(free_prev);
    selected  = seek_os(fd, 0, SEEK_END);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    if (write_os(fd, &free_prev, sizeof(PALLOC_OFFSET)) != sizeof(PALLOC_OFFSET)) {
      perror("palloc::write");
      return 0;
    }
    free_prev = be64toh(free_prev);
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
    marker    = htobe64(size | PALLOC_MARKER_FREE);
    free_next = htobe64(selected + size + (sizeof(PALLOC_SIZE)*2));
    free_prev = htobe64(selected);
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
    marker = htobe64((selected_size - size - (sizeof(PALLOC_SIZE)*2)) | PALLOC_MARKER_FREE);
    free_pprev = htobe64(seek_os(fd, 0, SEEK_CUR));
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
    seek_os(fd, (be64toh(marker) & (~PALLOC_MARKER_FREE)) - (sizeof(PALLOC_OFFSET)*2), SEEK_CUR);
    if (write_os(fd, &marker, sizeof(PALLOC_SIZE)) != sizeof(PALLOC_SIZE)) {
      perror("palloc::write");
      return 0;
    }
    // Update next block's pointer
    free_nnext = be64toh(free_nnext);
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
  free_prev = be64toh(free_prev);
  free_next = be64toh(free_next);
  if (free_prev) {
    free_next = htobe64(free_next);
    seek_os(fd, free_prev + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    write_os(fd, &free_next, sizeof(PALLOC_OFFSET));
    free_next = be64toh(free_next);
  }
  if (free_next) {
    free_prev = htobe64(free_prev);
    seek_os(fd, free_next + sizeof(PALLOC_SIZE), SEEK_SET);
    write_os(fd, &free_prev, sizeof(PALLOC_OFFSET));
    free_prev = be64toh(free_prev);
  }

  // Fix first free
  if (finfo->first_free == selected) {
    finfo->first_free = free_next;
  }

  // Mark selected block as non-free
  marker = htobe64(size);
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

/* uint64_t palloc(struct palloc_t *pt, size_t size) { */
/*   uint64_t marker_h; */
/*   uint64_t marker_be; */
/*   uint64_t marker_left; */
/*   uint64_t marker_right; */
/*   uint64_t free_prev; */
/*   uint64_t free_next; */
/*   uint64_t z = 0; */
/*   ssize_t n; */
/*   size = MAX(16,size); */

/*   // Fetch the first free block */
/*   if (!(pt->first_free)) { */
/*     lseek_os(pt->descriptor, pt->header_size, SEEK_SET); */

/*     while(1) { */
/*       n = read_os(pt->descriptor, &marker_be, sizeof(marker_be)); */
/*       if (n < 0) { */
/*         perror("palloc::read"); */
/*         return 0; */
/*       } */
/*       if (n == 0) { */
/*         // EOF */
/*         break; */
/*       } */

/*       // Convert to our host format */
/*       marker_h = be64toh(marker_be); */

/*       // If free, we found one */
/*       if (marker_h & PALLOC_MARKER_FREE) { */
/*         pt->first_free = lseek_os(pt->descriptor, 0 - sizeof(marker_be), SEEK_CUR); */
/*         break; */
/*       } */

/*       // Here = not free, skip to next */
/*       lseek_os(pt->descriptor, marker_h + sizeof(marker_be), SEEK_CUR); */
/*     } */
/*   } */

/*   // No first_free & non-dynamic = full */
/*   if ((!pt->first_free) && (!(pt->flags & PALLOC_DYNAMIC))) { */
/*     return 0; */
/*   } */

/*   // No first free = allocate more space */
/*   if (!pt->first_free) { */
/*     pt->first_free = pt->size; */
/*     lseek_os(pt->descriptor, pt->size, SEEK_SET); */
/*     marker_be = htobe64(PALLOC_MARKER_FREE | ((uint64_t)size)); */
/*     write_os(pt->descriptor, &marker_be, sizeof(marker_be));             // Start marker */
/*     write_os(pt->descriptor, &z, sizeof(z));                             // Previous free pointer (zero, 'cuz no first_free) */
/*     write_os(pt->descriptor, &z, sizeof(z));                             // Next free pointer */
/*     lseek_os(pt->descriptor, size - (sizeof(marker_be)*2), SEEK_CUR); // Skip remainder of marker */
/*     write_os(pt->descriptor, &marker_be, sizeof(marker_be));             // End marker */
/*     pt->size = lseek_os(pt->descriptor, 0, SEEK_CUR);                 // Update tracked file size */
/*   } */

/*   // Here = we got first_free */

/*   // Look for a free blob that is large enough */
/*   uint64_t found_free = 0; */
/*   lseek_os(pt->descriptor, pt->first_free, SEEK_SET); // Go to the first free */
/*   while(1) { */
/*     n = read_os(pt->descriptor, &marker_be, sizeof(marker_be)); */
/*     if (n < 0) { */
/*       perror("palloc::read"); */
/*       return 0; */
/*     } */
/*     if (n == 0) { */
/*       // No free space, regardless of dynamicness */
/*       return 0; */
/*     } */
/*     marker_h = be64toh(marker_be) & (~PALLOC_MARKER_FREE); */
/*     // Found marker */
/*     if (marker_h >= size) { */
/*       found_free = lseek_os(pt->descriptor, 0 - sizeof(marker_be), SEEK_CUR); */
/*       break; */
/*     } */
/*     // Skip to next free blob */
/*     lseek_os(pt->descriptor, sizeof(marker_be), SEEK_CUR);   // Skip to "next free" pointer */
/*     n = read_os(pt->descriptor, &marker_be, sizeof(marker_be)); // Read the pointer */
/*     marker_h = be64toh(marker_be); */
/*     if (!marker_h) return 0;                                 // Handle full medium */
/*     lseek_os(pt->descriptor, marker_h, SEEK_SET);            // Move to next free */
/*   } */

/*   // Here = we got found_free */

/*   // Get size of found_free */
/*   lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block */
/*   read_os(pt->descriptor, &marker_be, sizeof(marker_be)); */
/*   marker_h = be64toh(marker_be) & (~PALLOC_MARKER_FREE); */

/*   // Split blob if it's large enough to contain another */
/*   if ((marker_h - size) > (sizeof(marker_be)*4)) { */
/*     marker_left  = htobe64(((uint64_t)size) | PALLOC_MARKER_FREE); */
/*     marker_right = htobe64((marker_h - size - (sizeof(marker_be)*2)) | PALLOC_MARKER_FREE); */

/*     // Write left markers */
/*     lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block */
/*     write_os(pt->descriptor, &marker_left, sizeof(marker_be)); // Write start marker */
/*     lseek_os(pt->descriptor, size, SEEK_CUR);       // Skip data */
/*     write_os(pt->descriptor, &marker_left, sizeof(marker_be)); // Write end marker */

/*     // Write right markers */
/*     write_os(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write start marker */
/*     lseek_os(pt->descriptor, marker_h - size - (sizeof(marker_be)*2), SEEK_CUR); // Skip data */
/*     write_os(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write end marker */

/*     // Get a readable address of the right free block */
/*     marker_right = found_free + (sizeof(marker_be)*2) + size; */

/*     // Move next free pointer over */
/*     lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer src */
/*     read_os(pt->descriptor, &free_next, sizeof(free_next)); */
/*     lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer dst */
/*     write_os(pt->descriptor, &free_next, sizeof(free_next)); */

/*     // Fix next free pointer in left block */
/*     free_next = htobe64(marker_right); */
/*     lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer left */
/*     write_os(pt->descriptor, &free_next, sizeof(free_next)); */

/*     // Fix prev free pointer in right block */
/*     free_prev = htobe64(found_free); */
/*     lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)), SEEK_SET); // Move to prev free pointer right */
/*     write_os(pt->descriptor, &free_prev, sizeof(free_prev)); */

/*     // And update the remembered marker of the pointer-to block */
/*     marker_h = size; */
/*   } */

/*   // Update previous free block's next pointer */
/*   lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET); */
/*   read_os(pt->descriptor, &free_prev, sizeof(free_prev)); */
/*   read_os(pt->descriptor, &free_next, sizeof(free_next)); */
/*   free_prev = be64toh(free_prev); */
/*   if (free_prev) { */
/*     lseek_os(pt->descriptor, free_prev + (sizeof(marker_be)*2), SEEK_SET); */
/*     write_os(pt->descriptor, &free_next, sizeof(free_next)); */
/*   } */

/*   // Update next block's prev pointer */
/*   lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET); */
/*   read_os(pt->descriptor, &free_prev, sizeof(free_prev)); */
/*   read_os(pt->descriptor, &free_next, sizeof(free_next)); */
/*   free_next = be64toh(free_next); */
/*   if (free_next) { */
/*     lseek_os(pt->descriptor, free_next + (sizeof(marker_be)*1), SEEK_SET); */
/*     write_os(pt->descriptor, &free_prev, sizeof(free_prev)); */
/*   } */

/*   // Move first_free tracker if needed */
/*   if (found_free == pt->first_free) { */
/*     pt->first_free = free_next; */
/*   } */

/*   // Mark found_free as occupied */
/*   lseek_os(pt->descriptor, found_free, SEEK_SET); */
/*   marker_be = htobe64(marker_h); */
/*   write_os(pt->descriptor, &marker_be, sizeof(marker_be)); // Start marker */
/*   lseek_os(pt->descriptor, marker_h, SEEK_CUR);         // Skip content */
/*   write_os(pt->descriptor, &marker_be, sizeof(marker_be)); // End marker */

/*   // Return pointer to the content */
/*   return found_free + sizeof(marker_be); */
/* } */

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
  left_marker = htobe64(left_size | PALLOC_MARKER_FREE);
  seek_os(fd, left, SEEK_SET);
  write_os(fd, &left_marker, sizeof(left_marker));
  seek_os(fd, sizeof(PALLOC_OFFSET), SEEK_CUR);
  write_os(fd, &right_next, sizeof(right_next));
  seek_os(fd, left_size - (sizeof(PALLOC_OFFSET)*2), SEEK_CUR);
  write_os(fd, &left_marker, sizeof(left_marker));

  // Update right_next's prev pointer
  if (be64toh(right_next)) {
    left = htobe64(left);
    seek_os(fd, be64toh(right_next) + sizeof(PALLOC_SIZE), SEEK_SET);
    write_os(fd, &left, sizeof(left));
    left = be64toh(left);
  }
}

PALLOC_RESPONSE pfree(PALLOC_FD fd, PALLOC_OFFSET ptr) {
  PALLOC_SIZE marker, size;
  PALLOC_OFFSET off_left, off_right;

  // Convert pointer to outer
  ptr -= sizeof(PALLOC_SIZE);

  // Fetch info
  struct palloc_fd_info *finfo = _palloc_info(fd);

  // Detect free neighbours
  PALLOC_OFFSET free_prev = 0;
  PALLOC_OFFSET free_next = 0;
  PALLOC_OFFSET free_cur  = finfo->first_free;
  while(free_cur) {
    if (free_cur < ptr)   free_prev = free_cur;
    if (free_cur > ptr) { free_next = free_cur; break; }
    seek_os(fd, free_cur + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    read_os(fd, &free_cur, sizeof(PALLOC_OFFSET));
    free_cur = be64toh(free_cur);
  }

  // We need BE pointers during the next block
  free_prev = htobe64(free_prev);
  free_next = htobe64(free_next);

  // Mark ourselves as free & link into free list
  seek_os(fd, ptr, SEEK_SET);
  read_os(fd, &marker, sizeof(PALLOC_SIZE));
  size   = be64toh(marker) & (~PALLOC_MARKER_FREE);
  marker = htobe64(size | PALLOC_MARKER_FREE);
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
  free_prev = be64toh(free_prev);
  free_next = be64toh(free_next);

  // Update our neighbours' pointers
  if (free_prev) {
    seek_os(fd, free_prev + sizeof(PALLOC_SIZE) + sizeof(PALLOC_OFFSET), SEEK_SET);
    off_left = htobe64(ptr);
    write_os(fd, &off_left, sizeof(PALLOC_SIZE));
  }
  if (free_next) {
    seek_os(fd, free_next + sizeof(PALLOC_SIZE), SEEK_SET);
    off_right = htobe64(ptr);
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
    marker = be64toh(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return ptr + sizeof(marker);

  // Convert pointer to internal usage
  } else {
    ptr -= sizeof(PALLOC_SIZE);
  }

  // Read the marker of the given block
  seek_os(fd, ptr, SEEK_SET);
  if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) return 0;
  marker = be64toh(marker);

  // Skip the first one
  ptr = ptr + (sizeof(PALLOC_SIZE) * 2) + (marker & (~PALLOC_MARKER_FREE));
  while(1) {
    if (ptr >= finfo->medium_size) return 0;
    seek_os(fd, ptr, SEEK_SET);
    if (read_os(fd, &marker, sizeof(marker)) != sizeof(marker)) return 0;
    marker = be64toh(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return ptr + sizeof(marker);
    ptr += (sizeof(marker)*2) + (marker & (~PALLOC_MARKER_FREE));
  }

  return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
