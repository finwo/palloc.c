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

#if defined(_WIN32) || defined(_WIN64)
#define stat_os __stat64
#define fstat_os _fstat64
#define lseek_os _lseeki64
#define open_os _open
#define write_os _write
#define read_os _read
#define O_CREAT _O_CREAT
#define O_RDWR  _O_RDWR
#define OPENMODE  (_S_IREAD | _S_IWRITE)
#define O_DSYNC 0
#define ssize_t SSIZE_T
#elif defined(__APPLE__)
#define stat_os stat
#define fstat_os fstat
#define lseek_os lseek
#define open_os open
#define write_os write
#define read_os read
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#else
#define stat_os stat64
#define fstat_os fstat64
#define lseek_os lseek64
#define open_os open
#define write_os write
#define read_os read
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#endif

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

const char *expected_header = "PBA\0";

struct palloc_t * palloc_init(const char *filename, uint32_t flags) {
  char *filepath = canonical_path(filename);
  char *z = calloc(48, sizeof(char));
  char *hdr = malloc(8);
  uint64_t tmp;

  if (!filepath) {
    perror("palloc_init::realpath");
    free(z);
    free(hdr);
    return NULL;
  }

  // Prep the response
  struct palloc_t *pt = malloc(sizeof(struct palloc_t));
  pt->filename   = filepath;
  pt->descriptor = 0;
  pt->flags      = flags;
  pt->first_free = 0;
  pt->size       = 0;

  int openFlags = O_RDWR | O_CREAT;
#if defined(__APPLE__) || defined(_WIN32) || defined(_WIN64)
  // No O_LARGEFILE needed
#else
  openFlags |= O_LARGEFILE;
#endif
  if (flags & PALLOC_SYNC) {
    openFlags |= O_DSYNC;
  }
  pt->descriptor = open_os(pt->filename, openFlags, OPENMODE);
  if (pt->descriptor < 0) {
    perror("palloc_init::open");
    palloc_close(pt);
    free(z);
    free(hdr);
    return NULL;
  }

  struct stat_os fst;
  if (fstat_os(pt->descriptor, &fst)) {
    perror("palloc_init::fstat");
    palloc_close(pt);
    free(z);
    free(hdr);
    return NULL;
  }
  pt->size = fst.st_size;

  // Make sure the medium has room for the header
  if (pt->size < 8) {
    if (flags & PALLOC_DYNAMIC) {
      write_os(pt->descriptor, z, 8);
      pt->size = 8;
      lseek_os(pt->descriptor, 0, SEEK_SET);
    } else {
      fprintf(stderr, "Incompatible medium: %s\n", pt->filename);
      palloc_close(pt);
      free(z);
      free(hdr);
      return NULL;
    }
  }

  // Check for pre-existing header & flags
  read_os(pt->descriptor, hdr, 4);
  if (strncmp(expected_header, hdr, 4)) {

    // Initialize medium: header is missing
    lseek_os(pt->descriptor, 0, SEEK_SET);
    write_os(pt->descriptor, expected_header, 4);
    pt->flags = htobe32(pt->flags);
    write_os(pt->descriptor, &(pt->flags), sizeof(pt->flags));
    pt->flags = be32toh(pt->flags);
    lseek_os(pt->descriptor, 0, SEEK_SET);

    // TODO: generalize this
    // TODO: support extended headers
    if (pt->flags & PALLOC_EXTENDED) {
      // Reserved for future use
    } else {
      pt->header_size = 4 + sizeof(pt->flags); // PBA\0 + flags
    }

    // Grow medium if incompatible size detected
    if ((pt->size > 8) && (pt->size < 40)) {
      lseek_os(pt->descriptor, 8, SEEK_SET);
      write_os(pt->descriptor, z, 32);
      pt->size = 40;
    }

    // Mark whole medium as free if there's space
    if ( pt->size >= 40) {
      // Mark the whole medium as free
      lseek_os(pt->descriptor, 8, SEEK_SET);
      tmp = htobe64(PALLOC_MARKER_FREE | (pt->size - pt->header_size - sizeof(tmp) - sizeof(tmp)));
      write_os(pt->descriptor, &tmp, sizeof(tmp));
      lseek_os(pt->descriptor, pt->size - sizeof(tmp), SEEK_SET);
      write_os(pt->descriptor, &tmp, sizeof(tmp));
    }

  } else {
    // Read flags from file
    read_os(pt->descriptor, &(pt->flags), sizeof(pt->flags));
    pt->flags = be32toh(pt->flags);
    lseek_os(pt->descriptor, 0, SEEK_SET);

    // TODO: generalize this
    // TODO: support extended headers
    if (pt->flags & PALLOC_EXTENDED) {
      // Reserved for future use
    } else {
      pt->header_size = 4 + sizeof(pt->flags); // PBA\0 + flags
    }
  }

  free(z);
  free(hdr);
  return pt;
}

void palloc_close(struct palloc_t *pt) {
  if (!pt) return;

  if (pt->filename) free(pt->filename);
  if (pt->descriptor >= 0) close(pt->descriptor);
  free(pt);
}

uint64_t palloc(struct palloc_t *pt, size_t size) {
  uint64_t marker_h;
  uint64_t marker_be;
  uint64_t marker_left;
  uint64_t marker_right;
  uint64_t free_prev;
  uint64_t free_next;
  uint64_t z = 0;
  ssize_t n;
  size = MAX(16,size);

  // Fetch the first free block
  if (!(pt->first_free)) {
    lseek_os(pt->descriptor, pt->header_size, SEEK_SET);

    while(1) {
      n = read_os(pt->descriptor, &marker_be, sizeof(marker_be));
      if (n < 0) {
        perror("palloc::read");
        return 0;
      }
      if (n == 0) {
        // EOF
        break;
      }

      // Convert to our host format
      marker_h = be64toh(marker_be);

      // If free, we found one
      if (marker_h & PALLOC_MARKER_FREE) {
        pt->first_free = lseek_os(pt->descriptor, 0 - sizeof(marker_be), SEEK_CUR);
        break;
      }

      // Here = not free, skip to next
      lseek_os(pt->descriptor, marker_h + sizeof(marker_be), SEEK_CUR);
    }
  }

  // No first_free & non-dynamic = full
  if ((!pt->first_free) && (!(pt->flags & PALLOC_DYNAMIC))) {
    return 0;
  }

  // No first free = allocate more space
  if (!pt->first_free) {
    pt->first_free = pt->size;
    lseek_os(pt->descriptor, pt->size, SEEK_SET);
    marker_be = htobe64(PALLOC_MARKER_FREE | ((uint64_t)size));
    write_os(pt->descriptor, &marker_be, sizeof(marker_be));             // Start marker
    write_os(pt->descriptor, &z, sizeof(z));                             // Previous free pointer (zero, 'cuz no first_free)
    write_os(pt->descriptor, &z, sizeof(z));                             // Next free pointer
    lseek_os(pt->descriptor, size - (sizeof(marker_be)*2), SEEK_CUR); // Skip remainder of marker
    write_os(pt->descriptor, &marker_be, sizeof(marker_be));             // End marker
    pt->size = lseek_os(pt->descriptor, 0, SEEK_CUR);                 // Update tracked file size
  }

  // Here = we got first_free

  // Look for a free blob that is large enough
  uint64_t found_free = 0;
  lseek_os(pt->descriptor, pt->first_free, SEEK_SET); // Go to the first free
  while(1) {
    n = read_os(pt->descriptor, &marker_be, sizeof(marker_be));
    if (n < 0) {
      perror("palloc::read");
      return 0;
    }
    if (n == 0) {
      // No free space, regardless of dynamicness
      return 0;
    }
    marker_h = be64toh(marker_be) & (~PALLOC_MARKER_FREE);
    // Found marker
    if (marker_h >= size) {
      found_free = lseek_os(pt->descriptor, 0 - sizeof(marker_be), SEEK_CUR);
      break;
    }
    // Skip to next free blob
    lseek_os(pt->descriptor, sizeof(marker_be), SEEK_CUR);   // Skip to "next free" pointer
    n = read_os(pt->descriptor, &marker_be, sizeof(marker_be)); // Read the pointer
    marker_h = be64toh(marker_be);
    if (!marker_h) return 0;                                 // Handle full medium
    lseek_os(pt->descriptor, marker_h, SEEK_SET);            // Move to next free
  }

  // Here = we got found_free

  // Get size of found_free
  lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block
  read_os(pt->descriptor, &marker_be, sizeof(marker_be));
  marker_h = be64toh(marker_be) & (~PALLOC_MARKER_FREE);

  // Split blob if it's large enough to contain another
  if ((marker_h - size) > (sizeof(marker_be)*4)) {
    marker_left  = htobe64(((uint64_t)size) | PALLOC_MARKER_FREE);
    marker_right = htobe64((marker_h - size - (sizeof(marker_be)*2)) | PALLOC_MARKER_FREE);

    // Write left markers
    lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block
    write_os(pt->descriptor, &marker_left, sizeof(marker_be)); // Write start marker
    lseek_os(pt->descriptor, size, SEEK_CUR);       // Skip data
    write_os(pt->descriptor, &marker_left, sizeof(marker_be)); // Write end marker

    // Write right markers
    write_os(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write start marker
    lseek_os(pt->descriptor, marker_h - size - (sizeof(marker_be)*2), SEEK_CUR); // Skip data
    write_os(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write end marker

    // Get a readable address of the right free block
    marker_right = found_free + (sizeof(marker_be)*2) + size;

    // Move next free pointer over
    lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer src
    read_os(pt->descriptor, &free_next, sizeof(free_next));
    lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer dst
    write_os(pt->descriptor, &free_next, sizeof(free_next));

    // Fix next free pointer in left block
    free_next = htobe64(marker_right);
    lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer left
    write_os(pt->descriptor, &free_next, sizeof(free_next));

    // Fix prev free pointer in right block
    free_prev = htobe64(found_free);
    lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)), SEEK_SET); // Move to prev free pointer right
    write_os(pt->descriptor, &free_prev, sizeof(free_prev));

    // And update the remembered marker of the pointer-to block
    marker_h = size;
  }

  // Update previous free block's next pointer
  lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET);
  read_os(pt->descriptor, &free_prev, sizeof(free_prev));
  read_os(pt->descriptor, &free_next, sizeof(free_next));
  free_prev = be64toh(free_prev);
  if (free_prev) {
    lseek_os(pt->descriptor, free_prev + (sizeof(marker_be)*2), SEEK_SET);
    write_os(pt->descriptor, &free_next, sizeof(free_next));
  }

  // Update next block's prev pointer
  lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET);
  read_os(pt->descriptor, &free_prev, sizeof(free_prev));
  read_os(pt->descriptor, &free_next, sizeof(free_next));
  free_next = be64toh(free_next);
  if (free_next) {
    lseek_os(pt->descriptor, free_next + (sizeof(marker_be)*1), SEEK_SET);
    write_os(pt->descriptor, &free_prev, sizeof(free_prev));
  }

  // Move first_free tracker if needed
  if (found_free == pt->first_free) {
    pt->first_free = free_next;
  }

  // Mark found_free as occupied
  lseek_os(pt->descriptor, found_free, SEEK_SET);
  marker_be = htobe64(marker_h);
  write_os(pt->descriptor, &marker_be, sizeof(marker_be)); // Start marker
  lseek_os(pt->descriptor, marker_h, SEEK_CUR);         // Skip content
  write_os(pt->descriptor, &marker_be, sizeof(marker_be)); // End marker

  // Return pointer to the content
  return found_free + sizeof(marker_be);
}

void _pfree_merge(struct palloc_t *pt, uint64_t left, uint64_t right) {
  uint64_t left_marker , left_prev , left_next , left_size ;
  uint64_t right_marker, right_prev, right_next, right_size;

  // Fetch info on left block
  lseek_os(pt->descriptor, left, SEEK_SET);
  read_os(pt->descriptor, &left_marker, sizeof(left_marker));
  read_os(pt->descriptor, &left_prev, sizeof(left_prev));
  read_os(pt->descriptor, &left_next, sizeof(left_next));
  left_marker = be64toh(left_marker);
  left_size   = left_marker & (~PALLOC_MARKER_FREE);

  // Fetch info on right block
  lseek_os(pt->descriptor, right, SEEK_SET);
  read_os(pt->descriptor, &right_marker, sizeof(right_marker));
  read_os(pt->descriptor, &right_prev, sizeof(right_prev));
  read_os(pt->descriptor, &right_next, sizeof(right_next));
  right_marker = be64toh(right_marker);
  right_size   = right_marker & (~PALLOC_MARKER_FREE);

  // Not both free = do not merge
  if (!(left_marker & right_marker & PALLOC_MARKER_FREE)) {
    return;
  }

  // Not consecutive = do not merge
  if ((left + left_size + (sizeof(uint64_t)*2)) != right) {
    return;
  }

  // Actually merge the blocks into 1 big one
  left_size   = left_size + right_size + (sizeof(uint64_t)*2);
  left_marker = htobe64(left_size | PALLOC_MARKER_FREE);
  lseek_os(pt->descriptor, left, SEEK_SET);
  write_os(pt->descriptor, &left_marker, sizeof(left_marker));
  lseek_os(pt->descriptor, sizeof(uint64_t), SEEK_CUR);
  write_os(pt->descriptor, &right_next, sizeof(right_next));
  lseek_os(pt->descriptor, left_size - (sizeof(uint64_t)*2), SEEK_CUR);
  write_os(pt->descriptor, &left_marker, sizeof(left_marker));

  // Update right_next's prev pointer
  if (right_next) {
    left = htobe64(left);
    lseek_os(pt->descriptor, be64toh(right_next) + sizeof(uint64_t), SEEK_SET);
    write_os(pt->descriptor, &left, sizeof(left));
    left = be64toh(left);
  }
}

void pfree(struct palloc_t *pt, uint64_t ptr) {
  uint64_t marker_left, marker_right, marker, size;
  uint64_t free_cur, free_prev, free_next;

  // Fix offset
  ptr -= sizeof(marker);

  // Detect pre-existing free blocks around us
  free_prev = 0;
  free_next = 0;
  free_cur  = pt->first_free;
  while(free_cur) {
    if (free_cur < ptr) free_prev = free_cur;
    if (free_cur > ptr) { free_next = free_cur; break; }
    lseek_os(pt->descriptor, free_cur + (sizeof(marker)*2), SEEK_SET);
    read_os(pt->descriptor, &free_cur, sizeof(free_cur));
    free_cur = be64toh(free_cur);
  }

  // We need BE pointers during the next block
  free_prev = htobe64(free_prev);
  free_next = htobe64(free_next);

  // Mark ourselves as free & write our pointers
  lseek_os(pt->descriptor, ptr, SEEK_SET);
  read_os(pt->descriptor, &marker, sizeof(marker));
  size   = be64toh(marker) & (~PALLOC_MARKER_FREE);
  marker = htobe64(size | PALLOC_MARKER_FREE);
  lseek_os(pt->descriptor, ptr, SEEK_SET);
  write_os(pt->descriptor, &marker, sizeof(marker));
  write_os(pt->descriptor, &free_prev, sizeof(free_prev));
  write_os(pt->descriptor, &free_next, sizeof(free_next));
  lseek_os(pt->descriptor, size - (sizeof(marker)*2), SEEK_CUR);
  write_os(pt->descriptor, &marker, sizeof(marker));

  // Update first_free if needed
  if ((!(pt->first_free)) || (pt->first_free > ptr)) {
    pt->first_free = ptr;
  }

  // We need H pointers during the next block
  free_prev = be64toh(free_prev);
  free_next = be64toh(free_next);

  // Update our neighbours' pointers
  if (free_prev) {
    lseek_os(pt->descriptor, free_prev + (sizeof(marker)*2), SEEK_SET);
    marker_left = htobe64(ptr);
    write_os(pt->descriptor, &marker_left, sizeof(marker));
  }
  if (free_next) {
    lseek_os(pt->descriptor, free_next + sizeof(marker), SEEK_SET);
    marker_right = htobe64(ptr);
    write_os(pt->descriptor, &marker_right, sizeof(marker));
  }

  // Merge with neighbours if consecutive
  // Next first, so we don't need to update our tracking
  if (free_next) _pfree_merge(pt, ptr, free_next);
  if (free_prev) _pfree_merge(pt, free_prev, ptr);

  // TODO: if dynamic and we're last in the file, truncate
}

// Gets the size of the blob
uint64_t palloc_size(struct palloc_t *pt, uint64_t ptr) {
  uint64_t marker;
  lseek_os(pt->descriptor, ptr - sizeof(marker), SEEK_SET);
  if (read_os(pt->descriptor, &marker, sizeof(marker)) <= 0) return 0;
  return be64toh(marker) & (~PALLOC_MARKER_FREE);
}

// Fetches the first allocated
uint64_t palloc_first(struct palloc_t *pt) {
  uint64_t marker;
  uint64_t idx = pt->header_size;

  while(1) {
    lseek_os(pt->descriptor, idx, SEEK_SET);
    if (read_os(pt->descriptor, &marker, sizeof(marker)) <= 0) return 0;
    marker = be64toh(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return idx + sizeof(marker);
    idx += (sizeof(marker)*2) + (marker & (~PALLOC_MARKER_FREE));
  }

}

// Fetches the next allocated
uint64_t palloc_next(struct palloc_t *pt, uint64_t ptr) {
  uint64_t marker;
  lseek_os(pt->descriptor, ptr - sizeof(marker), SEEK_SET);
  if (read_os(pt->descriptor, &marker, sizeof(marker)) <= 0) return 0;
  uint64_t idx = ptr + sizeof(marker) + (be64toh(marker) & (~PALLOC_MARKER_FREE));
  while(1) {
    lseek_os(pt->descriptor, idx, SEEK_SET);
    if (read_os(pt->descriptor, &marker, sizeof(marker)) <= 0) return 0;
    marker = be64toh(marker);
    if (!(marker & PALLOC_MARKER_FREE)) return idx + sizeof(marker);
    idx += (sizeof(marker)*2) + (marker & (~PALLOC_MARKER_FREE));
  }
}

#ifdef __cplusplus
} // extern "C"
#endif
