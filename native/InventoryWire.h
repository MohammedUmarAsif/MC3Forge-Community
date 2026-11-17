// Bounded inventory projection. Only CPU-thread code reads files or commits intents.
#pragma once
#include "LootWire.h"
#include <array>
#include <cmath>
#include <vector>
#include <unordered_set>

namespace MC3Forge
{
inline constexpr std::array<const char*,8> gear_slots{{"engine","nitrous","tires","brakes","transmission","suspension","accessory_1","accessory_2"}};
inline constexpr std::array<const char*,7> gear_keys{{"power","drift_nitro","drift_charge_rate","luck","loot_quantity","pity_rate","money"}};
inline bool wire_uint(const std::string& text,unsigned& value)
{
    const auto r=std::from_chars(text.data(),text.data()+text.size(),value);
    return !text.empty() && r.ec==std::errc{} && r.ptr==text.data()+text.size();
}
inline std::vector<std::string> split_wire(const std::string& text,char separator)
{
    std::vector<std::string> fields; std::size_t start=0;
    for (;;) { const auto at=text.find(separator,start); fields.push_back(text.substr(start,at-start)); if (at==std::string::npos) return fields; start=at+1; }
}
inline bool wire_float(const std::string& text,float& value)
{
    std::istringstream stream(text); stream.imbue(std::locale::classic()); stream>>std::noskipws>>value;
    return stream && stream.eof() && std::isfinite(value) && value>=0 && value<=1;
}
struct GearAffix { std::string key; float value=0; };
struct GearItem
{
    std::string id,kind,rarity,slot,label,offer_id,offer_key;
    std::vector<GearAffix> affixes;
    unsigned offer_index=0; float offer_value=0;
};
struct InventoryView
{
    unsigned revision=0,dry=0;
    std::string feedback;
    std::vector<GearItem> items;
    static bool parse(const std::string& text,InventoryView& out)
    {
        if (text.size()>1024*1024 || text.empty() || text.back()!='\n') return false;
        std::istringstream input(text); std::string line; InventoryView next;
        if (!std::getline(input,line) || line!="MC3FORGE_INVENTORY_V1" || !std::getline(input,line) || !wire_uint(line,next.revision) || next.revision>1000000 ||
            !std::getline(input,line) || !wire_uint(line,next.dry) || next.dry>=100) return false;
        if (!std::getline(input,next.feedback) || next.feedback.empty() || next.feedback.size()>160) return false;
        for (char c:next.feedback) if (c<32 || c>126) return false;
        std::array<bool,8> occupied{};
        std::unordered_set<std::string> identifiers;
        while (std::getline(input,line))
        {
            if (next.items.size()>=1000 || line.size()>2048) return false;
            const auto fields=split_wire(line,'|'); if (fields.size()!=7) return false;
            GearItem item; item.id=fields[0]; item.kind=fields[1]; item.rarity=fields[2]; item.slot=fields[3]; item.label=fields[6];
            if (!hex_id(item.id) || (item.label!="test-kit" && item.label!="dev-kit" && item.label!="earned")) return false;
            if (!identifiers.insert(item.id).second) return false;
            bool known_kind=item.kind=="accessory";
            for (unsigned i=0;i<6;++i) known_kind=known_kind || item.kind==gear_slots[i];
            if (!known_kind) return false;
            unsigned budget= item.rarity=="common" ? 1 : item.rarity=="uncommon" ? 2 : item.rarity=="rare" ? 3 : item.rarity=="epic" ? 4 : 0;
            if (!budget) return false;
            if (item.slot!="none")
            {
                bool known=false;
                for (unsigned i=0;i<gear_slots.size();++i) if (item.slot==gear_slots[i])
                { if (occupied[i] || (i<6 ? item.kind!=item.slot : item.kind!="accessory")) return false; occupied[i]=true; known=true; }
                if (!known) return false;
            }
            for (const auto& entry:split_wire(fields[4],','))
            {
                const auto pair=split_wire(entry,'='); GearAffix a;
                if (pair.size()!=2 || !wire_float(pair[1],a.value)) return false;
                a.key=pair[0]; bool known=false;
                for (const auto key:gear_keys) known=known || a.key==key;
                for (const auto& prior:item.affixes) if (prior.key==a.key) return false;
                if (!known || (a.key=="drift_nitro" && a.value!=1) || (a.key!="power" && a.key!="drift_nitro" && a.key!="money" && a.value>.5f)) return false;
                item.affixes.push_back(a);
            }
            if (item.affixes.size()!=budget && !(item.rarity=="epic" && item.affixes.size()==6)) return false;
            budget=static_cast<unsigned>(item.affixes.size());
            if (fields[5]!="none")
            {
                const auto offer=split_wire(fields[5],',');
                if (offer.size()!=4 || !hex_id(offer[0]) || !wire_uint(offer[1],item.offer_index) || item.offer_index>=budget || !wire_float(offer[3],item.offer_value)) return false;
                item.offer_id=offer[0]; item.offer_key=offer[2]; bool known=false;
                for (const auto key:gear_keys) known=known || item.offer_key==key;
                if (!known) return false;
            }
            next.items.push_back(std::move(item));
        }
        out=std::move(next); return true;
    }
};
struct GearAction
{
    std::string action,item="none",slot="engine",offer="none";
    unsigned affix=0,revision=0;
};
inline void write_gear_action(std::ostream& output,const std::string& session,unsigned sequence,const GearAction& a)
{
    output.imbue(std::locale::classic());
    output<<"{\"schema\":2,\"session\":\""<<session<<"\",\"sequence\":"<<sequence<<",\"action\":\""<<a.action
        <<"\",\"item\":\""<<a.item<<"\",\"slot\":\""<<a.slot<<"\",\"affix\":"<<a.affix<<",\"revision\":"<<a.revision<<",\"offer\":\""<<a.offer<<"\"}\n";
}
} // namespace MC3Forge
