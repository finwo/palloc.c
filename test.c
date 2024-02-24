#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "finwo/assert.h"

#include "palloc.h"

#if defined(_WIN32) || defined(_WIN64)
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
  uint64_t alloc_5;
  uint64_t alloc_6;
  uint64_t alloc_7;

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
  ASSERT("size of the alloc is indicated as 16", palloc_size(pt, my_alloc) == 16);

  my_alloc = palloc(pt, 32);
  ASSERT("first allocation is located at 48", my_alloc == 48);
  ASSERT("size after small alloc is 88", pt->size == 88);
  ASSERT("size of the alloc is indicated as 32", palloc_size(pt, my_alloc) == 32);
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
  pfree(pt, alloc_3);
  pfree(pt, alloc_0);
  pfree(pt, alloc_2);

  // Allocation on static medium works
  alloc_5 = palloc(pt, 40);
  ASSERT("6th allocation, after 3 freed at org alloc", alloc_5 == alloc_2);

  // Static medium has a free space, let's assign another 64 bytes and skip that one free
  alloc_6 = palloc(pt, 64);
  ASSERT("7th allocation, skipping gap, at 240", alloc_6 == 240);

  // Assigning more than available space should fail
  alloc_7 = palloc(pt, 1024*1024);
  ASSERT("8th allocation, being too large, fails", alloc_7 == 0);

  // Iteration
  ASSERT("1st is indicated as first allocated", palloc_first(pt) == alloc_1);
  ASSERT("2nd is indicated as filled gap", palloc_next(pt, alloc_1) == alloc_5);
  ASSERT("3nd is indicated as original 5th", palloc_next(pt, alloc_5) == alloc_4);
  ASSERT("4th is indicated as original 7th", palloc_next(pt, alloc_4) == alloc_6);
  ASSERT("5th is indicated as not existing", palloc_next(pt, alloc_6) == 0);

  my_alloc = palloc(pt, 1);
  ASSERT("1st is indicated as filled gap after new alloc", palloc_first(pt) == my_alloc);

  palloc_close(pt);

}

int main() {
  RUN(test_init);
  return TEST_REPORT();
}

#ifdef __cplusplus
} // extern "C"
#endif
