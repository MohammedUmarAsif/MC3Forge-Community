#pragma once
#include "TrainerCore.h"
#include <limits>

namespace MC3Forge
{
// Generalized experimental finish credit. Never credits an already-finished attachment.
class MoneyBonus
{
    bool armed=false, done=false;
    std::uint32_t race_id=0, wallet=0, cash_before=0;
    unsigned rate=0;
    Actor owner;
public:
    MoneyReport report;
    void invalidate() { armed=false; done=true; report.fault=true; report.reason="Reset/state load: money disabled until restart"; }
    template<class Journal> void tick(Memory& m,bool source_ok,unsigned permille,Journal journal)
    {
        if (report.fault) return;
        const auto root=m.word(0x618AE0), race=m.word(root+4), manager=m.word(0x619DB8);
        Actor observed;
        if (!source_ok || m.word(0x619B00)!=2 || !m.object(root,8) || !m.object(race,0x12CC) ||
            m.word(race)!=0x630770 || !m.object(manager,0x38) || m.word(manager)!=0x6304D8 || !actor(m,observed))
        { armed=false; done=false; race_id=0; report.reason="Money waiting for supported paid race"; return; }
        const auto state=m.word(manager+4), dispatch=m.word(manager+0x34);
        if (state<4 || race!=race_id)
        { armed=false; done=false; race_id=race; }
        const auto bank_owner=m.word(0x619E00), bank=m.word(bank_owner+0xC);
        if (!m.object(bank_owner,0x10) || !m.object(bank,0xAC4) || permille>8000)
        { report.reason="Money wallet/rate guard refused"; armed=false; done=true; return; }
        const auto address=bank+0xAC0, cash=m.word(address);
        if (cash>0x7fffffff) { armed=false; done=true; report.reason="Money signed-wallet guard refused"; return; }
        const bool finished=m.bytes[race+0x98]==1;
        if (state==4 && dispatch==4 && !finished && !done)
        {
            if (!armed)
            {
                if (m.bytes[race+0xE] || m.word(race+0x12C0) || m.word(race+0x12C8))
                { done=true; report.reason="Money refuses pre-existing finish/payout"; return; }
                armed=true; wallet=address; cash_before=cash; rate=permille; owner=observed; ++report.attempt;
                report.reason="Money armed: observing native finish credit";
            }
            else if (wallet!=address || cash!=cash_before || rate!=permille || !owner.same(observed))
            { armed=false; done=true; report.reason="Money changed before finish: attempt refused"; }
            return;
        }
        if (!armed || done || state!=7 || dispatch!=7 || !finished) return;
        if (m.bytes[race+0xE]==0 || cash==cash_before)
        { report.reason="Money waiting for native payout to settle"; return; }
        done=true; armed=false;
        if (wallet!=address || !owner.same(observed) || rate!=permille || m.bytes[race+0xE]!=1 ||
            m.word(race+0x12C0)<1 || !(m.word(race+0x12C8)&1) || m.word(race+0xA8)<1 || m.word(race+0xA8)>8 ||
            cash<=cash_before || cash-cash_before>500000)
        { report.reason="Money finish/ownership/positive payout guard refused"; return; }
        report.before=cash_before; report.native_after=cash; report.base=cash-cash_before; report.permille=rate;
        const std::uint64_t bonus=static_cast<std::uint64_t>(report.base)*rate/1000;
        if (static_cast<std::uint64_t>(cash)+bonus>0x7fffffff)
        { report.reason="Money overflow refused"; return; }
        report.bonus=static_cast<unsigned>(bonus); report.after=cash+report.bonus;
        report.reason="Money prepared";
        if (!journal(false,report)) { report.fault=true; report.reason="Money intent journal failed: no write"; return; }
        // Same CPU boundary, no guest execution between the durable intent and readback.
        if (m.word(address)!=cash) { report.fault=true; report.reason="Money wallet changed after intent: no write"; return; }
        if (report.bonus) std::memcpy(m.bytes+address,&report.after,4);
        if (m.word(address)!=report.after) { report.fault=true; report.reason="Money readback failed: never retry automatically"; return; }
        ++report.credits; report.reason="Money credited and read back";
        if (!journal(true,report)) { report.fault=true; report.reason="Money applied but receipt failed: never retry automatically"; }
    }
};
}
