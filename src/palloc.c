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
#include <unistd.h>

#include "finwo/endian.h"

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

#ifdef WIN32
#define stat_os _stat64
#define fstat_os fstat64
#define lseek_os lseek64
#elif defined(__APPLE__)
#define stat_os stat
#define fstat_os fstat
#define lseek_os lseek
#else
#define stat_os stat64
#define fstat_os fstat64
#define lseek_os lseek64
#endif

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

const char *expected_header = "PBA\0";


// Origin: https://stackoverflow.com/a/60923396/2928176
////////////////////////////////////////////////////////////////////////////////
// Return the input path in a canonical form. This is achieved by expanding all
// symbolic links, resolving references to "." and "..", and removing duplicate
// "/" characters.
//
// If the file exists, its path is canonicalized and returned. If the file,
// or parts of the containing directory, do not exist, path components are
// removed from the end until an existing path is found. The remainder of the
// path is then appended to the canonical form of the existing path,
// and returned. Consequently, the returned path may not exist. The portion
// of the path which exists, however, is represented in canonical form.
//
// If successful, this function returns a C-string, which needs to be freed by
// the caller using free().
//
// ARGUMENTS:
//   file_path
//   File path, whose canonical form to return.
//
// RETURNS:
//   On success, returns the canonical path to the file, which needs to be freed
//   by the caller.
//
//   On failure, returns NULL.
////////////////////////////////////////////////////////////////////////////////
char *_realpath(const char *file_path) {
  char *canonical_file_path  = NULL;
  unsigned int file_path_len = strlen(file_path);

  if (file_path_len > 0) {
    canonical_file_path = realpath(file_path, NULL);
    if (canonical_file_path == NULL && errno == ENOENT) {
      // The file was not found. Back up to a segment which exists,
      // and append the remainder of the path to it.
      char *file_path_copy = NULL;
      if (file_path[0] == '/'                ||
          (strncmp(file_path, "./", 2) == 0) ||
          (strncmp(file_path, "../", 3) == 0)
      ) {
        // Absolute path, or path starts with "./" or "../"
        file_path_copy = strdup(file_path);
      } else {
        // Relative path
        file_path_copy = (char*)malloc(strlen(file_path) + 3);
        strcpy(file_path_copy, "./");
        strcat(file_path_copy, file_path);
      }

      // Remove path components from the end, until an existing path is found
      for (int char_idx = strlen(file_path_copy) - 1;
           char_idx >= 0 && canonical_file_path == NULL;
           --char_idx
      ) {
        if (file_path_copy[char_idx] == '/') {
          // Remove the slash character
          file_path_copy[char_idx] = '\0';

          canonical_file_path = realpath(file_path_copy, NULL);
          if (canonical_file_path != NULL) {
            // An existing path was found. Append the remainder of the path
            // to a canonical form of the existing path.
            char *combined_file_path = (char*)malloc(strlen(canonical_file_path) + strlen(file_path_copy + char_idx + 1) + 2);
            strcpy(combined_file_path, canonical_file_path);
            strcat(combined_file_path, "/");
            strcat(combined_file_path, file_path_copy + char_idx + 1);
            free(canonical_file_path);
            canonical_file_path = combined_file_path;
          } else {
            // The path segment does not exist. Replace the slash character
            // and keep trying by removing the previous path component.
            file_path_copy[char_idx] = '/';
          }
        }
      }

      free(file_path_copy);
    }
  }

  return canonical_file_path;
}



struct palloc_t * palloc_init(const char *filename, uint32_t flags) {
  char *filepath = _realpath(filename);
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
#if defined(__APPLE__)
  // No O_LARGEFILE needed
#else
  openFlags |= O_LARGEFILE;
#endif
  int openMode  = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP; // 0660
  if (flags & PALLOC_SYNC) {
    openFlags |= O_DSYNC;
  }
  pt->descriptor = open(pt->filename, openFlags, openMode);
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
      write(pt->descriptor, z, 8);
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
  read(pt->descriptor, hdr, 4);
  if (strncmp(expected_header, hdr, 4)) {

    // Initialize medium: header is missing
    lseek_os(pt->descriptor, 0, SEEK_SET);
    write(pt->descriptor, expected_header, 4);
    pt->flags = htobe32(pt->flags);
    write(pt->descriptor, &(pt->flags), sizeof(pt->flags));
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
      write(pt->descriptor, z, 32);
      pt->size = 40;
    }

    // Mark whole medium as free if there's space
    if ( pt->size >= 40) {
      // Mark the whole medium as free
      lseek_os(pt->descriptor, 8, SEEK_SET);
      tmp = htobe64(PALLOC_MARKER_FREE | (pt->size - pt->header_size - sizeof(tmp) - sizeof(tmp)));
      write(pt->descriptor, &tmp, sizeof(tmp));
      lseek_os(pt->descriptor, pt->size - sizeof(tmp), SEEK_SET);
      write(pt->descriptor, &tmp, sizeof(tmp));
    }

  } else {
    // Read flags from file
    read(pt->descriptor, &(pt->flags), sizeof(pt->flags));
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
      n = read(pt->descriptor, &marker_be, sizeof(marker_be));
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
    write(pt->descriptor, &marker_be, sizeof(marker_be));             // Start marker
    write(pt->descriptor, &z, sizeof(z));                             // Previous free pointer (zero, 'cuz no first_free)
    write(pt->descriptor, &z, sizeof(z));                             // Next free pointer
    lseek_os(pt->descriptor, size - (sizeof(marker_be)*2), SEEK_CUR); // Skip remainder of marker
    write(pt->descriptor, &marker_be, sizeof(marker_be));             // End marker
    pt->size = lseek_os(pt->descriptor, 0, SEEK_CUR);                 // Update tracked file size
  }

  // Here = we got first_free

  // Look for a free blob that is large enough
  uint64_t found_free = 0;
  lseek_os(pt->descriptor, pt->first_free, SEEK_SET); // Go to the first free
  while(1) {
    n = read(pt->descriptor, &marker_be, sizeof(marker_be));
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
    n = read(pt->descriptor, &marker_be, sizeof(marker_be)); // Read the pointer
    marker_h = be64toh(marker_be);
    if (!marker_h) return 0;                                 // Handle full medium
    lseek_os(pt->descriptor, marker_h, SEEK_SET);            // Move to next free
  }

  // Here = we got found_free

  // Get size of found_free
  lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block
  read(pt->descriptor, &marker_be, sizeof(marker_be));
  marker_h = be64toh(marker_be) & (~PALLOC_MARKER_FREE);

  // Split blob if it's large enough to contain another
  if ((marker_h - size) > (sizeof(marker_be)*4)) {
    marker_left  = htobe64(((uint64_t)size) | PALLOC_MARKER_FREE);
    marker_right = htobe64((marker_h - size - (sizeof(marker_be)*2)) | PALLOC_MARKER_FREE);

    // Write left markers
    lseek_os(pt->descriptor, found_free, SEEK_SET); // Move to found block
    write(pt->descriptor, &marker_left, sizeof(marker_be)); // Write start marker
    lseek_os(pt->descriptor, size, SEEK_CUR);       // Skip data
    write(pt->descriptor, &marker_left, sizeof(marker_be)); // Write end marker

    // Write right markers
    write(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write start marker
    lseek_os(pt->descriptor, marker_h - size - (sizeof(marker_be)*2), SEEK_CUR); // Skip data
    write(pt->descriptor, &marker_right, sizeof(marker_be));                             // Write end marker

    // Get a readable address of the right free block
    marker_right = found_free + (sizeof(marker_be)*2) + size;

    // Move next free pointer over
    lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer src
    read(pt->descriptor, &free_next, sizeof(free_next));
    lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer dst
    write(pt->descriptor, &free_next, sizeof(free_next));

    // Fix next free pointer in left block
    free_next = htobe64(marker_right);
    lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*2), SEEK_SET); // Move to next free pointer left
    write(pt->descriptor, &free_next, sizeof(free_next));

    // Fix prev free pointer in right block
    free_prev = htobe64(found_free);
    lseek_os(pt->descriptor, marker_right + (sizeof(marker_be)), SEEK_SET); // Move to prev free pointer right
    write(pt->descriptor, &free_prev, sizeof(free_prev));

    // And update the remembered marker of the pointer-to block
    marker_h = size;
  }

  // Update previous free block's next pointer
  lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET);
  read(pt->descriptor, &free_prev, sizeof(free_prev));
  read(pt->descriptor, &free_next, sizeof(free_next));
  free_prev = be64toh(free_prev);
  if (free_prev) {
    lseek_os(pt->descriptor, free_prev + (sizeof(marker_be)*2), SEEK_SET);
    write(pt->descriptor, &free_next, sizeof(free_next));
  }

  // Update next block's prev pointer
  lseek_os(pt->descriptor, found_free + (sizeof(marker_be)*1), SEEK_SET);
  read(pt->descriptor, &free_prev, sizeof(free_prev));
  read(pt->descriptor, &free_next, sizeof(free_next));
  free_next = be64toh(free_next);
  if (free_next) {
    lseek_os(pt->descriptor, free_next + (sizeof(marker_be)*1), SEEK_SET);
    write(pt->descriptor, &free_prev, sizeof(free_prev));
  }

  // Move first_free tracker if needed
  if (found_free == pt->first_free) {
    pt->first_free = free_next;
  }

  // Mark found_free as occupied
  lseek_os(pt->descriptor, found_free, SEEK_SET);
  marker_be = htobe64(marker_h);
  write(pt->descriptor, &marker_be, sizeof(marker_be)); // Start marker
  lseek_os(pt->descriptor, marker_h, SEEK_CUR);         // Skip content
  write(pt->descriptor, &marker_be, sizeof(marker_be)); // End marker

  // Return pointer to the content
  return found_free + sizeof(marker_be);
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
    read(pt->descriptor, &free_cur, sizeof(free_cur));
    free_cur = be64toh(free_cur);
  }

  // We need BE pointers during the next block
  free_prev = htobe64(free_prev);
  free_next = htobe64(free_next);

  // Mark ourselves as free & write our pointers
  lseek_os(pt->descriptor, ptr, SEEK_SET);
  read(pt->descriptor, &marker, sizeof(marker));
  size   = be64toh(marker) & (~PALLOC_MARKER_FREE);
  marker = htobe64(size | PALLOC_MARKER_FREE);
  lseek_os(pt->descriptor, ptr, SEEK_SET);
  write(pt->descriptor, &marker, sizeof(marker));
  write(pt->descriptor, &free_prev, sizeof(free_prev));
  write(pt->descriptor, &free_next, sizeof(free_next));
  lseek_os(pt->descriptor, size - (sizeof(marker)*2), SEEK_CUR);
  write(pt->descriptor, &marker, sizeof(marker));

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
    write(pt->descriptor, &marker_left, sizeof(marker));
  }
  if (free_next) {
    lseek_os(pt->descriptor, free_next + sizeof(marker), SEEK_SET);
    marker_right = htobe64(ptr);
    write(pt->descriptor, &marker_right, sizeof(marker));
  }

  /*   // Merge left if consecutive */
  /*   if (free_prev) { */
  /*     lseek_os(pt->descriptor, be64toh(free_prev), SEEK_SET); // Read size of free_prev */
  /*     read(pt->descriptor, &marker_left, sizeof(marker)); */
  /*     marker_left = be64toh(marker_left) & (~PALLOC_MARKER_FREE); */
  /*     if (be64toh(free_prev) + marker_left + (sizeof(marker)*3) == ptr) { // Only update if it's direct neighbour = us */
  /*       lseek_os(pt->descriptor, be64toh(free_prev) + (sizeof(marker)*1), SEEK_SET); // Update it's next */
  /*       read(pt->descriptor, &free_prev, sizeof(free_prev)); */
  /*       write(pt->descriptor, &free_next, sizeof(free_next)); */
  /*       size   = marker_left + size + (sizeof(marker)*2);                     // We are now what used to be free_prev */
  /*       ptr    = lseek_os(pt->descriptor, 0 - (sizeof(marker)*2), SEEK_CUR); */
  /*       marker = htobe64(size | PALLOC_MARKER_FREE); */
  /*       lseek_os(pt->descriptor, ptr - sizeof(marker), SEEK_SET);             // Update markers */
  /*       write(pt->descriptor, &marker, sizeof(marker)); */
  /*       lseek_os(pt->descriptor, size, SEEK_CUR); */
  /*       write(pt->descriptor, &marker, sizeof(marker)); */
  /*     } */
  /*   } */

  /*   // Merge right if consecutive */
  /*   if (free_next) { */
  /*     if ((ptr + size + sizeof(marker)) == be64toh(free_next)) { */
  /*       lseek_os(pt->descriptor, be64toh(free_next), SEEK_SET);      // Read size of free_next */
  /*       read(pt->descriptor, &marker_right, sizeof(marker_right)); */
  /*       marker_right = be64toh(marker_left) & (~PALLOC_MARKER_FREE); */
  /*       lseek_os(pt->descriptor, sizeof(marker), SEEK_CUR);          // And it's free_next */
  /*       read(pt->descriptor, &free_next, sizeof(free_next)); */
  /*       lseek_os(pt->descriptor, ptr + sizeof(marker), SEEK_SET);    // Update our own free_next */
  /*       write(pt->descriptor, &free_next, sizeof(free_next)); */
  /*       size   = marker_right + size + (sizeof(marker)*2);           // Update our markers */
  /*       marker = htobe64(size | PALLOC_MARKER_FREE); */
  /*       lseek_os(pt->descriptor, ptr - sizeof(marker), SEEK_SET); */
  /*       write(pt->descriptor, &marker, sizeof(marker)); */
  /*       lseek_os(pt->descriptor, size, SEEK_CUR); */
  /*       write(pt->descriptor, &marker, sizeof(marker)); */
  /*     } */
  /*   } */

  // TODO: if dynamic and we're last in the file, truncate
}

uint64_t palloc_size(struct palloc_t *pt, uint64_t ptr) {
  uint64_t marker;
  lseek_os(pt->descriptor, ptr - sizeof(marker), SEEK_SET);
  if (read(pt->descriptor, &marker, sizeof(marker)) <= 0) return 0;
  return be64toh(marker) & (~PALLOC_MARKER_FREE);
}

#ifdef __cplusplus
} // extern "C"
#endif
