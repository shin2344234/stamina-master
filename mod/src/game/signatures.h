#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.03.00
// (exe 1.0.0.2944). The table machinery is Flight Freedom's, which took it from
// Master Looter. Everything about stamina came out of the research in
// private/FEASIBILITY.md.
namespace us::sig
{
    // --- Static data tables -------------------------------------------------
    // Most table resolvers are clones of one template, told apart by the
    // `lea r8, [rip+"<tablename>"]` inside them. The prologue below is the
    // 16-bit-key flavour, which vehicleinfo, iteminfo, conditioninfo and
    // statusinfo all use.
    //
    // **Not every table is reached this way, and skill is one that is not.**
    // The exe holds "skill" at 0x055C3F70 with exactly one reference, a
    // `lea rax, "skill"; ret` accessor stub followed by a `mov eax, 0x81B00A7A;
    // ret` hash stub, so there is no resolver clone naming it and GlobalFor
    // will return 0 for it however long it is given. That is what the RTTI
    // route below is for. Do not read a GlobalFor failure on skill as the table
    // not being loaded yet.
    inline constexpr const char* kSig_LeaR8Rip = "4C 8D 05 ?? ?? ?? ??";
    inline constexpr const char* kSig_TableResolver16 =
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC ?? 0F B7 39 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    // Table object: u32 row count at +0x08, def[] pointer at +0x50 or +0x58
    // depending on build. Both are tried and whichever yields readable string
    // keys wins. A def's _stringKey is an engine string at +0x08.
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    inline constexpr unsigned kOff_Def_StringKey = 0x08;

    inline constexpr const char* kStr_StatusTable = "statusinfo";

    // The RTTI route, for tables with no resolver clone. Every static table has
    // a manager class, 150 of them named *InfoManager@pa@@ in the image.
    inline constexpr const char* kRtti_StatusManager = ".?AVStatusInfoManager@pa@@";
    inline constexpr const char* kRtti_SkillManager  = ".?AVSkillInfoManager@pa@@";

    // How far into a def record to look. Skill records run to about 1,550
    // bytes in the extracted files, so 0x800 covers all but the largest and
    // 0x400 would not.
    inline constexpr unsigned kDefScanBytes = 0x800;


    // --- Stamina ------------------------------------------------------------
    // What the extracted tables for this build say, so the probe can report a
    // disagreement instead of quietly trusting an offset. Regenerate these from
    // scripts/ after a game patch and do not assume they carried over: the
    // 2.03.00 patch changed 130 of 268 static tables.
    inline constexpr uint32_t kStatusRows = 84;
    inline constexpr uint32_t kSkillRows  = 2069;

    // statusinfo keys. 1000026 is the stat itself. 1000037 is the rate the game
    // already uses to reduce stamina spend, granted by
    // Equip_Socket_StaminaUseDecreaseRate, Passive_Abyss_StaminaDecreaseRate
    // and Skill_HorseStamina.
    inline constexpr uint32_t kStatusKey_Stamina         = 1000026;
    inline constexpr uint32_t kStatusKey_StaminaUseDown  = 1000037;
    inline constexpr uint32_t kStatusKey_StaminaUseUp    = 1000064;
    inline constexpr uint32_t kStatusKey_Hp              = 1000000;

    inline constexpr const char* kStatusName_Stamina        = "Stamina";
    inline constexpr const char* kStatusName_StaminaUseDown = "Stamina_UseResourceDecreaseRate";

    // How many of the 2,069 skill rows carry kStatusKey_Stamina somewhere in
    // the record, counted over the extracted table. The probe reports the
    // runtime count against this; a mismatch means the record layout moved and
    // nothing should be written on that basis.
    inline constexpr uint32_t kSkillRowsWithStamina = 301;

    // Skill rows whose names settle whether the walk found the right table.
    // Sprint, climbing, swimming, the glider and the mount, one each.
    inline constexpr const char* kSkillKey_Sprint   = "Skill_BasicMoveLv4";
    inline constexpr const char* kSkillKey_Climb    = "Skill_PointClimbLv0";
    inline constexpr const char* kSkillKey_Swim     = "Skill_Swimming_Run";
    inline constexpr const char* kSkillKey_Glide    = "Skill_CrowWing";
    inline constexpr const char* kSkillKey_Horse    = "Skill_HorseStamina";
}
