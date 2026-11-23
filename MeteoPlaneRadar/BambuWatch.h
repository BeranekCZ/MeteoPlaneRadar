#pragma once
#include "PrintWatch.h"

void BambuWatch_Begin();
void BambuWatch_Tick();
// The display can read the graph revision together with the snapshot.
bool BambuWatch_Get(PrintInfo* out, uint32_t* historyRevision = nullptr);
int BambuWatch_History(int16_t* noz, int16_t* nozT, int16_t* bed, int16_t* bedT,
                       uint16_t* age, int max);
