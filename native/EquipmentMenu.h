#pragma once
#include "InventoryWire.h"
#include <algorithm>
#include <optional>

namespace MC3Forge
{
enum class EquipmentPage { Slots, Parts, Affixes };
struct EquipmentMenu
{
    bool open=false, help=false;
    EquipmentPage page=EquipmentPage::Slots;
    unsigned slot=0, affix=0, help_tab=0, help_label=0;
    unsigned offer_choice=0;
    std::string selected, notice;
    bool waiting=false;
    unsigned submitted_revision=0;
    double submitted_at=0;
    void back()
    {
        if (help) help=false;
        else if (page==EquipmentPage::Affixes) page=EquipmentPage::Parts;
        else if (page==EquipmentPage::Parts) { page=EquipmentPage::Slots; selected.clear(); }
        else open=false;
    }
};
inline const GearItem* equipped_part(const InventoryView& view,unsigned slot)
{
    for (const auto& item:view.items) if (item.slot==gear_slots[slot]) return &item;
    return nullptr;
}
inline bool fits_slot(const GearItem& item,unsigned slot)
{ return slot<6 ? item.kind==gear_slots[slot] : item.kind=="accessory"; }
inline const GearItem* selected_part(const InventoryView& view,const EquipmentMenu& menu)
{
    for (const auto& item:view.items) if (item.id==menu.selected && fits_slot(item,menu.slot)) return &item;
    return nullptr;
}
inline std::array<float,7> build_totals(const InventoryView& view,const GearItem* replacement=nullptr,unsigned slot=0)
{
    std::array<float,7> result{};
    auto add=[&](const GearItem& item) {
        for (const auto& a:item.affixes) for (unsigned k=0;k<gear_keys.size();++k)
            if (a.key==gear_keys[k]) result[k]+=a.value;
    };
    for (const auto& item:view.items)
        if (item.slot!="none" && (!replacement || (item.slot!=gear_slots[slot] && item.id!=replacement->id))) add(item);
    if (replacement) add(*replacement);
    for (auto& value:result) value=std::round(value*1000000)/1000000;
    return result;
}
inline const char* rarity_name(const std::string& key)
{
    if (key=="epic") return "Exotic";
    if (key=="common") return "Common";
    if (key=="uncommon") return "Uncommon";
    if (key=="rare") return "Rare";
    if (key=="divine") return "Divine";
    return "Ethereal";
}
inline constexpr std::array<const char*,8> slot_names{{"Engine","Nitrous","Tires","Brakes","Transmission","Suspension","Accessory 1","Accessory 2"}};
inline std::string part_name(const GearItem& item)
{
    if (item.label!="earned" && item.label!="test-kit" && item.label!="dev-kit") return item.label;
    std::string name="Accessory";
    for (unsigned i=0;i<6;++i) if (item.kind==gear_slots[i]) name=slot_names[i];
    return (item.label=="dev-kit" ? "DEV " : item.label=="test-kit" ? "Test " : "Rolled ")+name+" #"+item.id.substr(0,6);
}
inline constexpr std::array<const char*,7> affix_names{{"Power bonus","Dynamo units","Dynamo charge rate","Rarity luck","Extra-part quantity","Exotic pity acceleration","Race money bonus"}};
inline const char* affix_name(const std::string& key)
{
    for (unsigned i=0;i<gear_keys.size();++i) if (key==gear_keys[i]) return affix_names[i];
    return "Unknown effect";
}
// Sampled at the PS2 pad packet boundary. Hold capture through release after close.
struct EquipmentInputGate
{
    bool captured=false, draining=false, chord_held=false;
    bool filter(bool allowed,bool open,unsigned held,bool& request_open)
    {
        constexpr unsigned chord=(1u<<9)|(1u<<10); // PAD_START + PAD_L1 indices.
        const bool chord_now=(held&chord)==chord;
        if (allowed && !open && chord_now && !chord_held && !draining)
        { request_open=true; captured=true; }
        chord_held=chord_now;
        if (open) captured=true;
        if (!open && captured && !request_open) { captured=false; draining=true; }
        const bool suppress=captured || draining || request_open;
        if (draining && held==0) draining=false;
        return suppress;
    }
};
}
