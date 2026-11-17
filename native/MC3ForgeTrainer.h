#pragma once
#include <cstdint>
namespace MC3ForgeTrainer
{
void Tick();          // CPU thread only.
void Suspend();       // CPU thread, before shutdown/save.
void Invalidate();    // CPU thread, before reset/state load.
void Render();        // Existing GS-thread Dear ImGui renderer; no guest access.
bool CaptureEquipmentInput(std::uint32_t held); // CPU pad packet boundary, first controller only.
}
