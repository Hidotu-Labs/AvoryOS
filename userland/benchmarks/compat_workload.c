/* Real Win32 API exercise for Wine or another compatibility runner. GPL-2.0. */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
static HANDLE request, reply;
static DWORD WINAPI worker(void *unused) {
  (void)unused;
  for (unsigned i = 0; i < 1000; i++) {
    if (WaitForSingleObject(request, 10000) != WAIT_OBJECT_0)
      return 1;
    if (!SetEvent(reply))
      return 2;
  }
  return 0;
}
int main(void) {
  request = CreateEventA(NULL, FALSE, FALSE, NULL);
  reply = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!request || !reply)
    return 1;
  HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
  if (!thread)
    return 2;
  for (unsigned i = 0; i < 1000; i++)
    if (!SetEvent(request) ||
        WaitForSingleObject(reply, 10000) != WAIT_OBJECT_0)
      return 3;
  if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
    return 4;
  DWORD code = 1;
  if (!GetExitCodeThread(thread, &code) || code)
    return 5;
  CloseHandle(thread);
  CloseHandle(request);
  CloseHandle(reply);
  char temp[MAX_PATH], path[MAX_PATH];
  if (!GetTempPathA(sizeof(temp), temp) ||
      !GetTempFileNameA(temp, "avb", 0, path))
    return 6;
  HANDLE file =
      CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    DeleteFileA(path);
    return 7;
  }
  unsigned char data[4096], readback[4096] = {0};
  for (unsigned i = 0; i < sizeof(data); i++)
    data[i] = (unsigned char)i;
  DWORD done = 0;
  int failed = 0;
  if (!WriteFile(file, data, sizeof(data), &done, NULL) ||
      done != sizeof(data) || !FlushFileBuffers(file))
    failed = 1;
  if (SetFilePointer(file, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
    failed = 1;
  if (!ReadFile(file, readback, sizeof(readback), &done, NULL) ||
      done != sizeof(readback))
    failed = 1;
  for (unsigned i = 0; i < sizeof(data); i++)
    if (data[i] != readback[i])
      failed = 1;
  CloseHandle(file);
  if (failed)
    return 8;
  puts("AVORY_COMPAT_WORKLOAD_PASS events=1000 bytes=4096");
  return 0;
}
