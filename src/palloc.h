#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define PALLOC_DEFAULT       0
#define PALLOC_DYNAMIC       1
#define PALLOC_SYNC          2
#define PALLOC_EXTENDED   (1<<31) // Reserved for future use

struct palloc_t {
  char     *filename;
  int      descriptor;
  uint32_t flags;
  uint32_t header_size;
  uint64_t first_free;
  uint64_t size;
};

struct palloc_t * palloc_init(const char *filename, uint32_t flags);
void              palloc_close(struct palloc_t *pt);

uint64_t palloc(struct palloc_t *pt, size_t size);
void     pfree(struct palloc_t *pt, uint64_t ptr);

uint64_t palloc_size(struct palloc_t *pt, uint64_t ptr);
uint64_t palloc_first(struct palloc_t *pt);
uint64_t palloc_next(struct palloc_t *pt, uint64_t ptr);

#ifdef __cplusplus
} // extern "C"
#endif
