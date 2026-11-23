#include "PrintNotices.h"
#include "Config.h"
#include <Arduino.h>
#include <string.h>

namespace {
constexpr unsigned CAP = 12;
PrintNotice pending[CAP];
unsigned count = 0;
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

void removeAt(unsigned pos) {
  --count;
  memmove(pending + pos, pending + pos + 1, (count - pos) * sizeof(PrintNotice));
}
}
void PrintNotices_Post(uint8_t screen, PrintEvent event, const PrintInfo& info) {
  if (event == PE_NONE) return;
  PrintNotice n{screen, event, millis(), {}, {}};
  strlcpy(n.file, info.file, sizeof(n.file));
  strlcpy(n.message, info.message, sizeof(n.message));
  portENTER_CRITICAL(&mutex);
  if (count == CAP) {
    unsigned drop = 0;
    while (drop < count && pending[drop].screen != screen) ++drop;
    removeAt(drop < count ? drop : 0);
  }
  unsigned pos = count++;
  while (pos && (int32_t(n.at - pending[pos-1].at) < 0 ||
         (n.at == pending[pos-1].at && screen == SCREEN_PRINT_I && pending[pos-1].screen == SCREEN_BAMBU_I))) {
    pending[pos] = pending[pos-1];
    --pos;
  }
  pending[pos] = n;
  portEXIT_CRITICAL(&mutex);
}
bool PrintNotices_Take(PrintNotice& notice) {
  portENTER_CRITICAL(&mutex);
  const bool available = count != 0;
  if (available) { notice = pending[0]; removeAt(0); }
  portEXIT_CRITICAL(&mutex);
  return available;
}
void PrintNotices_Clear(uint8_t screen) {
  portENTER_CRITICAL(&mutex);
  for (unsigned i = count; i > 0; --i) if (pending[i-1].screen == screen) removeAt(i-1);
  portEXIT_CRITICAL(&mutex);
}
void PrintNotices_Expire(uint32_t now) {
  portENTER_CRITICAL(&mutex);
  while (count && int32_t(now - pending[0].at) >= (int32_t)PRINT_ALERT_MAX_MS) removeAt(0);
  portEXIT_CRITICAL(&mutex);
}
