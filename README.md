palloc.c
========

Simple embeddable persistent-allocation library for C

Allows your program to allocate and iterate over persistently allocated blobs
of data within a file or other file-like medium.


API
---

### Definitions

<details>
  <summary>PALLOC_DEFAULT</summary>
  Default flags to initialize palloc with, in case some compatibility flags
  are required after a future update.

```C
#define PALLOC_DEFAULT 0
```

</details>
<details>
  <summary>PALLOC_DYNAMIC</summary>
  Indicates a storage medium to be initialized as being dynamic. This flag
  is overridden by the medium if the medium has already been initialized.

```C
#define PALLOC_DYNAMIC 1
```

</details>
<details>
  <summary>PALLOC_SYNC</summary>
  During the initialization, open the medium in DSYNC (or os' equivalent)
  mode to provide some minor protection against things like power failures
  or disconnects.
```C
#define PALLOC_SYNC 2
```

</details>
<details>
  <summary>PALLOC_EXTENDED</summary>
  Reserved flag for future use if the current reserved space for flags
  becomes unsufficient.
```C
#define PALLOC_EXTENDED (1<<31)
```

</details>

### Structs

<details>
  <summary>palloc_t</summary>
  The main palloc descriptor, pass this along to all calls to the library
  so the library knows the medium's structure and other required
  information.
```C
struct palloc_t {
 char     *filename;
 int      descriptor;
 uint32_t flags;
 uint32_t header_size;
 uint64_t first_free;
 uint64_t size;
};
```

</details>

# Methods

<details>
  <summary>palloc_init(filename, flags)</summary>
  Opens a palloc medium and initializes it if not done so already.
```C
struct palloc_t * palloc_init(const char *filename, uint32_t flags);
```

</details>
<details>
  <summary>palloc_close(pt)</summary>
  Closes the descriptor and frees the palloc_t.
```C
void palloc_close(struct palloc_t *pt);
```

</details>
<details>
  <summary>palloc(pt,size)</summary>
  Allocates a new blob of the given size in the storage medium and returns
  an offset to the start of the data section you can use for your storage
  purposes.
```C
uint64_t palloc(struct palloc_t *pt, size_t size);
```

</details>
<details>
  <summary>pfree(pt, ptr)</summary>
  Marks the blob pointed to by ptr as being unused, allowing it to be
  re-used for future allocations and preventing it from being returned
  during iteration.
```C
void pfree(struct palloc_t *pt, uint64_t ptr);
```

</details>
