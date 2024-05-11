#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#include "finwo/assert.h"

#include "palloc.h"

#if defined(_WIN32) || defined(_WIN64)
// Needs to be AFTER winsock2 which is used for endian.h
#include <windows.h>
#include <io.h>
#include <BaseTsd.h>
#else
#include <unistd.h>
#endif

#include "finwo/io.h"

#if defined(_WIN32) || defined(_WIN64)
#define OPENMODE  (_S_IREAD | _S_IWRITE)
#elif defined(__APPLE__)
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#else
#define OPENMODE  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#endif

void test_init() {
  char *testfile = "pizza.db";
  char *z = calloc(1024*1024, sizeof(char));

  // Remove the file for this test
  if (unlink_os(testfile)) {
    if (errno != ENOENT) {
      perror("unlink");
    }
  }

  // Pre-open the file descriptor palloc will be operating on
  int fd = palloc_open(testfile, PALLOC_DEFAULT | PALLOC_DYNAMIC);

  // Test a freshly opened file
  int size = seek_os(fd, 0, SEEK_END);
  ASSERT("Freshly opening a new file returns a valid file descriptor", fd != 0);
  ASSERT("Freshly created db is still 0 in size", size == 0);
  palloc_close(fd);

  // Test re-opening a 0-length file
  fd = palloc_open(testfile, PALLOC_DEFAULT | PALLOC_DYNAMIC);
  size = seek_os(fd, 0, SEEK_END);
  ASSERT("Re-opening 0-length returned a valid file descriptor", fd != 0);
  ASSERT("Re-opened 0-length file is still 0", size == 0);

  // Initialize the new file
  PALLOC_RESPONSE ret = palloc_init(fd, PALLOC_DEFAULT | PALLOC_DYNAMIC);
  ASSERT("Initializing a blank file returns successful", ret == PALLOC_OK);
  size = seek_os(fd, 0, SEEK_END);
  ASSERT("Initializing a blank file makes it 8 bytes", size == 8);
  palloc_close(fd);

  // Re-opening empty storage
  fd = palloc_open(testfile, PALLOC_DEFAULT | PALLOC_DYNAMIC);
  ASSERT("Re-opening pre-initialized returns a valid file descriptor", fd != 0);
  size = seek_os(fd, 0, SEEK_END);
  ASSERT("File is still 8 bytes after opening", size == 8);

  // Allocation on dynamic medium grows the file
  PALLOC_OFFSET my_alloc = palloc(fd, 4);
  size = seek_os(fd, 0, SEEK_END);
  ASSERT("first allocation is located at 16", my_alloc == 16);
  ASSERT("size after small alloc is 40", size == 40);
  ASSERT("size of the alloc is indicated as 16", palloc_size(fd, my_alloc) == 16);

  my_alloc = palloc(fd, 32);
  size = seek_os(fd, 0, SEEK_END);
  ASSERT("first allocation is located at 48", my_alloc == 48);
  ASSERT("size after small alloc is 88", size == 88);
  ASSERT("size of the alloc is indicated as 32", palloc_size(fd, my_alloc) == 32);
  palloc_close(fd);

  // Write empty larger file to test with as medium
  fd = open_os(testfile, O_RDWR);
  seek_os(fd, 0, SEEK_SET);
  write_os(fd, z, 1024*1024);
  /* close_os(fd); */

  // Initialize larger medium
  palloc_init(fd, PALLOC_DEFAULT);
  size = seek_os(fd, 0, SEEK_END);
  /* ASSERT("pt returned non-null for dynamic new file", pt != NULL); */
  ASSERT("size of non-dynamic file is still 1M", size == (1024*1024));
  /* ASSERT("header of newly created file is 8", pt->header_size == 8); */

  PALLOC_OFFSET alloc_0;
  PALLOC_OFFSET alloc_1;
  PALLOC_OFFSET alloc_2;
  PALLOC_OFFSET alloc_3;
  PALLOC_OFFSET alloc_4;
  PALLOC_OFFSET alloc_5;
  PALLOC_OFFSET alloc_6;
  PALLOC_OFFSET alloc_7;

  // Allocation on static medium works
  alloc_0 = palloc(fd, 4);
  ASSERT("1st allocation is located at 16", alloc_0 == 16);

  // Allocation on static medium works
  alloc_1 = palloc(fd, 32);
  ASSERT("2nd allocation is located at 48", alloc_1 == 48);

  // Allocation on static medium works
  alloc_2 = palloc(fd, 32);
  ASSERT("3rd allocation is located at 96", alloc_2 == 96);

  // Allocation on static medium works
  alloc_3 = palloc(fd, 32);
  ASSERT("4th allocation is located at 144", alloc_3 == 144);

  // Allocation on static medium works
  alloc_4 = palloc(fd, 32);
  ASSERT("5th allocation is located at 192", alloc_4 == 192);

  // Free up a couple
  ASSERT("free(4) returns OK", pfree(fd, alloc_3) == PALLOC_OK);
  ASSERT("free(1) returns OK", pfree(fd, alloc_0) == PALLOC_OK);
  ASSERT("free(3) returns OK", pfree(fd, alloc_2) == PALLOC_OK);

  // Check blocks have been merged
  ASSERT("Consecutive free blocks have been merged", palloc_size(fd, alloc_2) == 32 + 32 + (sizeof(uint64_t)*2));

  // Allocation on static medium works
  alloc_5 = palloc(fd, 40);
  ASSERT("6th allocation, after 3 freed at org alloc", alloc_5 == alloc_2);

  // Static medium has a free space, let's assign another 64 bytes and skip that one free
  alloc_6 = palloc(fd, 64);
  ASSERT("7th allocation, skipping gap, at 240", alloc_6 == 240);

  // Assigning more than available space should fail
  alloc_7 = palloc(fd, 1024*1024);
  ASSERT("8th allocation, being too large, fails", alloc_7 == 0);

  // Iteration
  ASSERT("1st is indicated as first allocated", palloc_next(fd, 0      ) == alloc_1);
  ASSERT("2nd is indicated as filled gap"     , palloc_next(fd, alloc_1) == alloc_5);
  ASSERT("3nd is indicated as original 5th"   , palloc_next(fd, alloc_5) == alloc_4);
  ASSERT("4th is indicated as original 7th"   , palloc_next(fd, alloc_4) == alloc_6);
  ASSERT("5th is indicated as not existing"   , palloc_next(fd, alloc_6) == 0      );

  my_alloc = palloc(fd, 1);
  ASSERT("1st is indicated as filled gap after new alloc", palloc_next(fd, 0) == my_alloc);

  palloc_close(fd);

  free(z);
  return;
}

int main() {
  RUN(test_init);
  return TEST_REPORT();
}

#ifdef __cplusplus
} // extern "C"
#endif
