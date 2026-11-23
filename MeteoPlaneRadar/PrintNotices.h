#pragma once
#include "PrintWatch.h"

struct PrintNotice {
  uint8_t screen;
  PrintEvent event;
  uint32_t at;
  char file[48];
  char message[64];
};

// Accessed from both monitor tasks and the display loop.
void PrintNotices_Post(uint8_t screen, PrintEvent event, const PrintInfo& info);
bool PrintNotices_Take(PrintNotice& notice);
void PrintNotices_Clear(uint8_t screen);
void PrintNotices_Expire(uint32_t now);  // while the Settings screen owns the display
