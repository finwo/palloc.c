#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "finwo/assert.h"

#include "palloc.h"

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

void test_init() {
  char *testfile = "pizza.db";
  char *z = calloc(1024*1024, sizeof(char));

  // Remove the file for this test
  if (unlink(testfile)) {
    if (errno != ENOENT) {
      perror("unlink");
    }
  }

  // Initialize new file
  struct palloc_t *pt = palloc_init(testfile, PALLOC_DEFAULT | PALLOC_DYNAMIC);
  ASSERT("pt returned non-null for dynamic new file", pt != NULL);
  ASSERT("size of newly created file is 8", pt->size == 8);
  ASSERT("header of newly created file is 8", pt->header_size == 8);
  palloc_close(pt);

  // Re-opening empty storage
  pt = palloc_init(testfile, PALLOC_DEFAULT);
  ASSERT("pt returned non-null for default re-used file", pt != NULL);
  ASSERT("size of re-used file is still 8", pt->size == 8);
  ASSERT("flags were properly read from file", pt->flags == (PALLOC_DEFAULT | PALLOC_DYNAMIC));
  ASSERT("header of re-used file is 8", pt->header_size == 8);

  // Allocation on dynamic medium grows the file
  uint64_t my_alloc = palloc(pt, 4);
  ASSERT("first allocation is located at 16", my_alloc == 16);
  ASSERT("size after small alloc is 40", pt->size == 40);
  /* my_alloc = palloc(pt, 32); */
  /* ASSERT("first allocation is located at 48", my_alloc == 48); */
  /* ASSERT("size after small alloc is 96", pt->size == 96); */
  palloc_close(pt);

  // Write empty larger file to test with as medium
  int fd = open(testfile, O_RDWR);
  lseek_os(fd, 0, SEEK_SET);
  write(fd, z, 1024*1024);
  close(fd);

  // Initialize larger medium
  pt = palloc_init(testfile, PALLOC_DEFAULT);
  ASSERT("pt returned non-null for dynamic new file", pt != NULL);
  ASSERT("size of newly created file is 1M", pt->size == (1024*1024));
  ASSERT("header of newly created file is 8", pt->header_size == 8);
  palloc_close(pt);

}

int main() {
  RUN(test_init);
  return TEST_REPORT();
}

#ifdef __cplusplus
} // extern "C"
#endif
