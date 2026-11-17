// Pointer-free local interchange; never substitutes for guest ownership checks.
#pragma once
#include <charconv>
#include <cstring>
#include <locale>
#include <sstream>
#include <string>
#include <ostream>

namespace MC3Forge
{
inline bool hex_id(const std::string& value)
{
    if (value.size() != 32) return false;
    for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

struct RolledLoadout
{
    unsigned revision = 0, power = 0, items = 0, dynamo_milli = 1000;
    std::string item;
    std::string dynamo_item;
    bool dynamo = false;
    static bool parse(const std::string& text, RolledLoadout& out)
    {
        if (text.size() > 512 || text.empty() || text.back() != '\n') return false;
        std::istringstream input(text);
        std::string line;
        if (!std::getline(input, line)) return false;
        const bool v3 = line == "MC3FORGE_ROLLED_V3";
        const bool v2 = v3 || line == "MC3FORGE_ROLLED_V2";
        if (!v2 && line != "MC3FORGE_ROLLED_V1") return false;
        RolledLoadout next;
        auto number = [&](const char* prefix, unsigned& value)
        {
            if (!std::getline(input, line) || line.rfind(prefix, 0) != 0) return false;
            const auto start = line.data() + std::strlen(prefix), end = line.data() + line.size();
            const auto result = std::from_chars(start, end, value);
            return result.ec == std::errc{} && result.ptr == end;
        };
        if (!number("revision=", next.revision) || !std::getline(input, line) || line.rfind("item=", 0) != 0) return false;
        next.item = line.substr(5);
        if (next.item.empty()) return false;
        if (v2 && next.item == "none") next.item.clear();
        if ((!next.item.empty() && !hex_id(next.item)) || !number("power=", next.power) || !number("items=", next.items)) return false;
        if (v2)
        {
            if (!std::getline(input,line) || line.rfind("dynamo_item=",0)!=0) return false;
            next.dynamo_item = line.substr(12);
            if (next.dynamo_item.empty()) return false;
            if (next.dynamo_item == "none") next.dynamo_item.clear();
            unsigned enabled=0;
            if ((!next.dynamo_item.empty() && !hex_id(next.dynamo_item)) || !number("dynamo=",enabled) || enabled>1) return false;
            next.dynamo = enabled==1;
        }
        if (v3 && (!number("dynamo_milli=",next.dynamo_milli) || next.dynamo_milli>40000 || (next.dynamo != (next.dynamo_milli>0)))) return false;
        if (std::getline(input,line) || (!v3 && !next.revision) || next.revision>1000000 || (!v3 && !next.items) || next.items>1000 || (!v3 && next.revision<next.items)) return false;
        if (next.item.empty()) { if (!v2 || next.power!=0) return false; }
        else if (v3 ? next.power>800 : next.power!=10 && next.power!=25 && next.power!=50 && next.power!=100) return false;
        if ((next.dynamo && next.dynamo_item.empty()) || (!v3 && next.item.empty() && next.dynamo_item.empty())) return false;
        out = next; return true;
    }
    bool same(const RolledLoadout& other) const
    { return revision == other.revision && power == other.power && items == other.items && item == other.item && dynamo_item==other.dynamo_item && dynamo==other.dynamo && dynamo_milli==other.dynamo_milli; }
};

inline void write_finish(std::ostream& output, const std::string& session, unsigned sequence,
    int rank, unsigned max_rank, const std::string& car)
{
    output.imbue(std::locale::classic());
    output << "{\"schema\":1,\"session\":\"" << session << "\",\"sequence\":" << sequence
        << ",\"rank\":" << rank << ",\"max_rank\":" << max_rank << ",\"car\":\"" << car << "\",\"code_mask\":7}\n";
}

inline void write_equipment(std::ostream& output,const std::string& session,unsigned sequence,bool equip)
{
    output.imbue(std::locale::classic());
    output << "{\"schema\":1,\"session\":\"" << session << "\",\"sequence\":" << sequence
        << ",\"action\":\"" << (equip ? "equip_dynamo" : "unequip_dynamo") << "\"}\n";
}
} // namespace MC3Forge
