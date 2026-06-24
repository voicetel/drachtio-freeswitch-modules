// Host unit tests for modules/mod_*/simple_buffer.h (SimpleBuffer).
// These exercise the bugs that were fixed: reads must return the OLDEST written
// chunk (not the next write slot) even on a partial fill, getNextChunk() must not
// underflow when empty, and overwrite must drop the oldest chunk in FIFO order.
#include <cstring>
#include <cstdint>
#include "test_util.h"
#include "simple_buffer.h"

// Helper: a chunk is 4 bytes here; build a 4-byte chunk from a tag char.
static void chunk(char tag, char out[4]) { std::memset(out, tag, 4); }

TEST("partial fill: read returns oldest written chunk, in order") {
  SimpleBuffer b(4, 8);            // 4-byte chunks, capacity 8
  char a[4], c[4];
  chunk('A', a); chunk('C', c);
  b.add(a, 4);
  b.add(c, 4);
  CHECK_EQ(b.getNumItems(), 2u);

  char* p1 = b.getNextChunk();
  CHECK(p1 != nullptr);
  CHECK(p1 != nullptr && p1[0] == 'A');   // oldest first, NOT a write-slot
  char* p2 = b.getNextChunk();
  CHECK(p2 != nullptr && p2[0] == 'C');
  CHECK_EQ(b.getNumItems(), 0u);
}

TEST("empty buffer: getNextChunk returns null and does not underflow") {
  SimpleBuffer b(4, 8);
  CHECK(b.getNextChunk() == nullptr);
  CHECK_EQ(b.getNumItems(), 0u);
  // Call again past empty — must stay 0, not wrap to a huge unsigned value.
  CHECK(b.getNextChunk() == nullptr);
  CHECK_EQ(b.getNumItems(), 0u);
}

TEST("multi-chunk add splits by chunk size") {
  SimpleBuffer b(4, 8);
  char two[8];
  std::memset(two, 'X', 4);
  std::memset(two + 4, 'Y', 4);
  b.add(two, 8);                   // one add, two chunks
  CHECK_EQ(b.getNumItems(), 2u);
  CHECK(b.getNextChunk()[0] == 'X');
  CHECK(b.getNextChunk()[0] == 'Y');
}

TEST("non-multiple length is rejected") {
  SimpleBuffer b(4, 8);
  char odd[3] = {'z', 'z', 'z'};
  b.add(odd, 3);
  CHECK_EQ(b.getNumItems(), 0u);
}

TEST("overwrite when full drops oldest in FIFO order") {
  SimpleBuffer b(4, 3);            // capacity 3
  char c[4];
  for (char t = '1'; t <= '4'; t++) { chunk(t, c); b.add(c, 4); }  // 1,2,3,4
  CHECK_EQ(b.getNumItems(), 3u);  // saturated at capacity
  // Oldest ('1') was overwritten by '4'; remaining oldest->newest = 2,3,4.
  CHECK(b.getNextChunk()[0] == '2');
  CHECK(b.getNextChunk()[0] == '3');
  CHECK(b.getNextChunk()[0] == '4');
  CHECK(b.getNextChunk() == nullptr);
}

int main() { return tu::run(); }
