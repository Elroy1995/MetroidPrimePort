#include <musyx/hardware.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  // Assigning this function pointer also checks the ABI in clang-cl/LLP64 CI.
  void (*upload)(void*, uintptr_t, size_t, unsigned long, void (*)(size_t), unsigned long) = aramUploadData;
  unsigned char source[64];
  for (size_t i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(i ^ 0xa5);
  aramInit(0);
  const u8 id = aramAllocateStreamBuffer(sizeof(source));
  if (id == 0xff) abort();
  size_t size = 0;
  const uintptr_t address = aramGetStreamBufferAddress(id, &size);
  if (address == 0 || size != sizeof(source)) abort();
  upload(source, address, sizeof(source), 0, NULL, 0);
  if (memcmp(source, (void*)address, sizeof(source)) != 0) abort();
  memset((void*)address, 0, size);
  hwFlushStream(source, 0, sizeof(source), id, NULL, 0);
  if (memcmp(source, (void*)address, sizeof(source)) != 0) abort();
  aramFreeStreamBuffer(id);
  aramExit();
  puts("MusyX host-address DMA regression passed");
  return 0;
}
