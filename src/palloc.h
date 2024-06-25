/// palloc.c
/// ========
///
/// Simple embeddable persistent-allocation library for C
///
/// Allows your program to allocate and iterate over persistently allocated blobs
/// of data within a file or other file-like medium.
///
/// This library is designed to be simple (to use), not to break any speed
/// records. While performance improvements are welcome, keep simplicity in mind
/// when making contributions.
///
/// Example
/// -------
///
/// ```C
/// // Pre-open the file descriptor palloc will be operating on
/// int fd = palloc_open("path/to/file.db", PALLOC_DEFAULT | PALLOC_DYNAMIC);
/// palloc_init(fd, PALLOC_DEFAULT);
///
/// // Fetch the first allocated blob
/// PALLOC_OFFSET first = palloc_next(fd, 0);
///
/// // Fetch the second allocated blob
/// PALLOC_OFFSET second = palloc_next(fd, first);
///
/// // Allocate a new blob of 1024 bytes
/// PALLOC_OFFSET third = palloc(fd, 1024);
///
/// // Free the first blob
/// pfree(fd, first);
///
/// // Close file descriptor and clear internal cache on it
/// palloc_close(fd);
/// ```
///
/// Installation
/// ------------
///
/// This library makes use of [dep](https://github.com/finwo/dep) to manage it's
/// dependencies and exports.
///
/// ```sh
/// dep add finwo/palloc
/// ```
///
/// Dependencies:
///
/// - [finwo/assert](https://github.com/finwo/assert.h)
/// - [finwo/canonical-path](https://github.com/finwo/canonical-path.c)
/// - [finwo/endian](https://github.com/finwo/endian.h)

#ifdef __cplusplus
extern "C" {
#endif

///
/// API
/// ---

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

///
/// ### Definitions - Flags
///

/// <details>
///   <summary>PALLOC_DEFAULT</summary>
///
///   Default flags to initialize palloc with, in case some compatibility flags
///   are required after a future update.
///<C
#define PALLOC_DEFAULT 0
///>
/// </details>

/// <details>
///   <summary>PALLOC_DYNAMIC</summary>
///
///   Indicates a storage medium to be initialized as being dynamic. This flag
///   is overridden by the medium if the medium has already been initialized.
///<C
#define PALLOC_DYNAMIC 1
///>
/// </details>

/// <details>
///   <summary>PALLOC_SYNC</summary>
///
///   During the initialization, open the medium in DSYNC (or os' equivalent)
///   mode to provide some minor protection against things like power failures
///   or disconnects.
///<C
#define PALLOC_SYNC 2
///>
/// </details>

/// <details>
///   <summary>PALLOC_EXTENDED</summary>
///
///   Reserved flag for future use if the current reserved space for flags
///   becomes unsufficient.
///<C
#define PALLOC_EXTENDED (1<<31)
///>
/// </details>

///
/// ### Definitions - Types
///

/// <details>
///   <summary>PALLOC_FD</summary>
///
///   A reference to how the file descriptors for palloc look like
///<C
#define PALLOC_FD int
///>
/// </details>

/// <details>
///   <summary>PALLOC_FLAGS</summary>
///
///   A reference to how the file descriptors for palloc look like
///<C
#define PALLOC_FLAGS uint32_t
///>
/// </details>

/// <details>
///   <summary>PALLOC_RESPONSE</summary>
///
///   Common return-type, indicating errors and such
///<C
#define PALLOC_RESPONSE int
///>
/// </details>

/// <details>
///   <summary>PALLOC_OFFSET</summary>
///
///   Indicates an offset within the file descriptor
#ifndef PALLOC_OFFSET
///<C
#define PALLOC_OFFSET uint64_t
///>
#endif
/// </details>

/// <details>
///   <summary>PALLOC_SIZE</summary>
///
///   Indicates an size within the file descriptor
#ifndef PALLOC_SIZE
///<C
#define PALLOC_SIZE uint64_t
///>
#endif
/// </details>

///
/// ### Definitions - Responses
///

/// <details>
///   <summary>PALLOC_OK</summary>
///
///   Indicates no error was encountered
///<C
#define PALLOC_OK (0)
///>
/// </details>

/// <details>
///   <summary>PALLOC_ERR</summary>
///
///   Indicates a generic error without specification
///<C
#define PALLOC_ERR (-1)
///>
/// </details>

///
/// ### Methods
///

/// <details>
///   <summary>palloc_open(filename, flags)</summary>
///
///   Opens a palloc medium and returns it as a file descriptor both palloc and
///   the user can use.
///<C
PALLOC_FD palloc_open(const char *filename, PALLOC_FLAGS flags);
///>
/// </details>

/// <details>
///   <summary>palloc_init(fd, flags)</summary>
///
///   Initializes a pre-opened medium for use with palloc if not already
///   initialized
///<C
PALLOC_RESPONSE palloc_init(PALLOC_FD fd, PALLOC_FLAGS flags);
///>
/// </details>

/// <details>
///   <summary>palloc_close(fd)</summary>
///
///   Closes a pre-opened file descriptor
///<C
PALLOC_RESPONSE palloc_close(PALLOC_FD fd);
///>
/// </details>

/// <details>
///   <summary>palloc(fd,size)</summary>
///
///   Allocates a new blob of the given size in the storage medium and returns
///   an offset to the start of the data section you can use for your storage
///   purposes.
///<C
PALLOC_OFFSET palloc(PALLOC_FD fd, PALLOC_SIZE size);
///>
/// </details>

/// <details>
///   <summary>pfree(fd,ptr)</summary>
///
///   Marks the blob pointed to by ptr as being unused, allowing it to be
///   re-used for future allocations and preventing it from being returned
///   during iteration.
///<C
PALLOC_RESPONSE pfree(PALLOC_FD fd, PALLOC_OFFSET ptr);
///>
/// </details>

/// <details>
///   <summary>palloc_size(fd,ptr)</summary>
///
///   Returns the real size of the data section of the allocated blob pointed
///   to by ptr, not the originally requested size.
///<C
PALLOC_SIZE palloc_size(PALLOC_FD fd, PALLOC_OFFSET ptr);
///>
/// </details>

/// <details>
///   <summary>palloc_next(pt, ptr)</summary>
///
///   Returns an offset to the data section of the next allocated blob within
///   the descriptor based on the offset to a data section indicated by ptr, or
///   0 if no next allocated blob exists.
///<C
PALLOC_OFFSET palloc_next(PALLOC_FD fd, PALLOC_OFFSET ptr);
///>
/// </details>

#ifdef __cplusplus
} // extern "C"
#endif

///
/// File structure
/// --------------
///
/// - header
///     - 4B header "PBA\0"
///     - uint16_t  flags
/// - blobs
///     - 8B free + size
/// - size indicator: data only, excludes size indicator itself
/// - free flag:
///     - 1 = free
///     - 0 = occupied
/// - blob structure:
///     - free:
///         - 8B size | flag
///         - 8B pointer previous free block (0 = no previous free block)
///         - 8B pointer next free block (0 = no next free block)
///         - 8B size | flag
///     - occupied:
///         - 8B size
///         - &lt;data[n]&gt;
///         - 8B size

