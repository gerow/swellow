#include <alloca.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ilog(const char *format, ...) __attribute__((format(printf, 1, 2)));

void ilog(const char *format, ...) {
  va_list ap;

  // len plus '\0' '\n' we'll add
  size_t flen = strlen(format);
  size_t size = flen + 2;
  char *newfmt = alloca(size);
  strncpy(newfmt, format, size);
  newfmt[flen] = '\n';
  newfmt[flen + 1] = '\0';

  va_start(ap, format);
  vfprintf(stderr, newfmt, ap);
  va_end(ap);
}

int main(int argc, char **argv) {
  ilog("hello world");
  ilog("exiting");
}
