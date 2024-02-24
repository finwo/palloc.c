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
  uint64_t z = 0;
  ssize_t n;

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
      lseek_os(pt->descriptor, MAX(sizeof(marker_be)*2, marker_h) + sizeof(marker_be), SEEK_CUR);
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
    write(pt->descriptor, &marker_be, sizeof(marker_be));               // Start marker
    write(pt->descriptor, &z, sizeof(z));                               // Previous free pointer (zero, 'cuz no first_free)
    write(pt->descriptor, &z, sizeof(z));                               // Next free pointer
    if (size > (sizeof(marker_be)*2)) {
      lseek_os(pt->descriptor, size - (sizeof(marker_be)*2), SEEK_CUR); // Skip remainder of marker
    }
    write(pt->descriptor, &marker_be, sizeof(marker_be));               // End marker
    pt->size = lseek_os(pt->descriptor, 0, SEEK_CUR);                   // Update tracked file size
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

  // TODO: Split found_free, must leave marker_h as size

  // Mark found_free as occupied
  lseek_os(pt->descriptor, found_free, SEEK_SET);
  marker_be = htobe64(marker_h);
  write(pt->descriptor, &marker_be, sizeof(marker_be));                  // Start marker
  lseek_os(pt->descriptor, MAX(sizeof(marker_be)*2,marker_h), SEEK_CUR); // Skip content
  write(pt->descriptor, &marker_be, sizeof(marker_be));                  // End marker

  // Return pointer to the content
  return found_free + sizeof(marker_be);
}

void pfree(struct palloc_t *instance, uint64_t ptr) {

}

#ifdef __cplusplus
} // extern "C"
#endif
