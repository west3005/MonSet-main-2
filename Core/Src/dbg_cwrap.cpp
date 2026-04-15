#include "dbg_cwrap.h"
#include "debug_uart.hpp"
#include <cstring>

extern "C" void dbg_puts(const char* s) {
  if (!s) return;
  // raw() быстрее и не тянет форматирование
  DBG.raw(s);
}

extern "C" void dbg_puts_ln(const char* s) {
  if (!s) return;
  DBG.raw(s);
  DBG.raw("\r\n");
}
