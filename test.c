#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <unistd.h>

#include "finwo/assert.h"

#include "palloc.h"

void test_init() {
  char *testfile = "pizza.db";

  // Remove the file for this test
  if (unlink(testfile)) {
    if (errno != ENOENT) {
      perror("unlink");
    }
  }

  // Initialize it
  struct palloc_t *pt = palloc_init(testfile, PALLOC_DEFAULT | PALLOC_DYNAMIC);

  ASSERT("pt returned non-null for dynamic new file", pt != NULL);
  ASSERT("size of newly created file is 8", pt->size == 8);
  ASSERT("header of newly created file is 8", pt->header_size == 8);

  palloc_close(pt);

  pt = palloc_init(testfile, PALLOC_DEFAULT);

  ASSERT("pt returned non-null for default re-used file", pt != NULL);
  ASSERT("size of re-used file is still 8", pt->size == 8);
  ASSERT("flags were properly read from file", pt->flags == (PALLOC_DEFAULT | PALLOC_DYNAMIC));
  ASSERT("header of re-used file is 8", pt->header_size == 8);

  palloc_close(pt);
}

int main() {
  RUN(test_init);
  return TEST_REPORT();
}

#ifdef __cplusplus
} // extern "C"
#endif
