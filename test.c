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
  uint64_t my_alloc;
  uint64_t alloc_0;
  uint64_t alloc_1;
  uint64_t alloc_2;
  uint64_t alloc_3;
  uint64_t alloc_4;

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
  my_alloc = palloc(pt, 4);
  ASSERT("first allocation is located at 16", my_alloc == 16);
  ASSERT("size after small alloc is 40", pt->size == 40);

  my_alloc = palloc(pt, 32);
  ASSERT("first allocation is located at 48", my_alloc == 48);
  ASSERT("size after small alloc is 88", pt->size == 88);
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

  // Allocation on static medium works
  alloc_0 = palloc(pt, 4);
  ASSERT("1st allocation is located at 16", alloc_0 == 16);

  // Allocation on static medium works
  alloc_1 = palloc(pt, 32);
  ASSERT("2nd allocation is located at 48", alloc_1 == 48);

  // Allocation on static medium works
  alloc_2 = palloc(pt, 32);
  ASSERT("3rd allocation is located at 96", alloc_2 == 96);

  // Allocation on static medium works
  alloc_3 = palloc(pt, 32);
  ASSERT("4th allocation is located at 144", alloc_3 == 144);

  // Allocation on static medium works
  alloc_4 = palloc(pt, 32);
  ASSERT("5th allocation is located at 192", alloc_4 == 192);

  // Free up a couple
  pfree(pt, alloc_1);
  pfree(pt, alloc_3);
  pfree(pt, alloc_2);

  palloc_close(pt);

}

int main() {
  RUN(test_init);
  return TEST_REPORT();
}

#ifdef __cplusplus
} // extern "C"
#endif
