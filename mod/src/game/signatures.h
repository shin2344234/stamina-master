#pragma once
#include <cstdint>

// Byte patterns, offsets and RTTI names for Crimson Desert 2.04.00, the DLC
// build (exe 1.0.0.3392, Steam build 26308950). The table machinery is Flight
// Freedom's, which took it from Master Looter. Everything about stamina came
// out of the research in private/FEASIBILITY.md.
//
// The DLC build moved most of what this file names, and none of it is carried
// over on trust. The resolvers changed prologue, a def record now starts with
// its string key, and StatusInfo and SkillInfo were both reordered; the values
// below were read off the 3392 loaders (patch-dlc catalog\3392\stamina-master.md).
// The two skill list offsets and the whole UseResourceStat record did not
// move. The list reader moved again, to 0x01730D30, with the same body.
// Nothing here fits 1.0.0.2976 any longer.
namespace sm::sig
{
    // --- Static data tables -------------------------------------------------
    // Most table resolvers are clones of one template, told apart by the
    // `lea r8, [rip+"<tablename>"]` inside them. On 3392 every resolver takes
    // its row by value in edx and opens with `mov r11, rsp; mov [r11+8], rbx;
    // mov [r11+0x18], rbp; push rsi; push rdi; push r14; sub rsp, n; mov edi,
    // edx; mov rbx, [rip+global]`. The first three bytes and the mov edi,edx
    // are wildcarded because the three resolvers moved into `.code` encode
    // them as `49 89 E3` and `89 D7` instead of `4C 8B DC` and `8B FA`. 130
    // hits on 3392, statusinfo's at 0x3DCE80 with its global at 0x71ADB98.
    // The 2976 form (`0F B7 39`, a pointer to a u16 key) hits nothing.
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
        "?? ?? ?? 49 89 5B 08 49 89 6B 18 56 57 41 56 48 83 EC ?? ?? ?? 48 8B 1D";
    inline constexpr unsigned kOff_TableResolver_MovGlobal = 0x15; // `mov rbx, cs:<table global>`
    inline constexpr unsigned kMax_LeaToPrologue = 0x180;

    // Table object: u32 row count at +0x08, def[] pointer at +0x50 or +0x58
    // depending on build. Both are tried and whichever yields readable string
    // keys wins. A def's _stringKey is an engine string at +0x00 on 3392; it
    // was at +0x08 with the u32 _key at +0x00 until the DLC build swapped them
    // (StatusInfo's loader stores the string through the def pointer itself
    // and then the key at [rdi+8]).
    inline constexpr unsigned kOff_Table_Count   = 0x08;
    inline constexpr unsigned kOff_Table_DefsA   = 0x58;
    inline constexpr unsigned kOff_Table_DefsB   = 0x50;
    inline constexpr unsigned kOff_Def_StringKey = 0x00;

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
    // 2.03.00 patch changed 130 of 268 static tables. statusinfo went from 84
    // rows to 87 with the DLC; the skill count was not re-extracted for 3392.
    inline constexpr uint32_t kStatusRows = 87;
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

    // --- Record layouts, in memory -----------------------------------------
    // Recovered with private/research/dump_record_layout.py, which reads them
    // off the loader: every field read is a `lea rdx,[rsi+off]` and a `mov
    // r8d,size` in front of a call, with the field's own failure message right
    // after the branch. **These are runtime offsets into the def object and
    // have nothing to do with where a field sits in the packed file record.**
    //
    // Checked against the running 2976 game: the status key hash held
    // hashlittle over the lowercase status name, and the two rows below read
    // 0xC08A2354 and 0x0CC20117, which is what hashing those names offline
    // gives. On 3392 the hash moved to +0x38 and the probe's
    // "Stamina _statusIndex" line is the runtime check that it still matches.
    inline constexpr unsigned kRec_StatusBytes = 0xE0;   // measured def spacing
    inline constexpr unsigned kRec_SkillBytes  = 0x140;

    // 3392 loader, record in rdi: `lea rdx,[rdi+0x2c]; mov r8d,4` at
    // 0x171B996, `lea rdx,[rdi+0x38]` at 0x171B9FF, `lea rdx,[rdi+0x16]; mov
    // r8d,1` at 0x171B80E. They were +0x14, +0x54 and +0x61 on 2976, and +0x14
    // is now a run of byte flags, so a stale index offset reads a wrong number
    // rather than failing.
    inline constexpr unsigned kOff_Status_Index      = 0x2C; // u32, and it is the row index
    inline constexpr unsigned kOff_Status_KeyHash    = 0x38; // u32 hashlittle(lowercase name)
    inline constexpr unsigned kOff_Status_UsePercent = 0x16; // u8

    inline constexpr uint32_t kHash_Stamina        = 0xC08A2354u;
    inline constexpr uint32_t kHash_StaminaUseDown = 0x0CC20117u;

    inline constexpr unsigned kOff_Skill_UseResourceStatList       = 0xA8;
    inline constexpr unsigned kOff_Skill_UseDriverResourceStatList = 0xC8;

    // Candidates for telling a movement skill from a combat one. The costs
    // divide 68 to 138 that way and the split wants to be a setting, but a
    // keyword rule over the skill key gets the edges wrong in both directions:
    // Skill_Bow_RollShot, Skill_ShieldDash, Skill_MoveCutting_I and
    // Skill_Climb_DaggerAttack are attacks with movement words in their names,
    // and nothing in a name is load-bearing anyway. One of these fields may
    // separate them properly, which is what the survey is for.
    //
    // _applyType is more than survey data: Apply reads it to tell a mounted
    // entry from one on foot, and 1 means mounted. All eight moved on 3392
    // (loader at 0x171FD96..0x171FFBA); the 2976 +0x34 now lands between
    // _cooltime and _buffLevelList and would misfile every mount entry.
    inline constexpr unsigned kOff_Skill_Cooltime     = 0x30; // u32
    inline constexpr unsigned kOff_Skill_ApplyType    = 0x11; // u8, `lea rdx,[rdi+0x11]` at 0x171FDFF
    inline constexpr unsigned kOff_Skill_IsUiAllowed  = 0x10; // u8
    inline constexpr unsigned kOff_Skill_AllowLowRes  = 0x0E; // u8
    inline constexpr unsigned kOff_Skill_IsNoAlert    = 0x13; // u8
    inline constexpr unsigned kOff_Skill_DamageType   = 0x12; // u8
    inline constexpr unsigned kOff_Skill_UiType       = 0x0F; // u8
    inline constexpr unsigned kOff_Skill_MaxLevel     = 0x24; // u32

    // A list field is {items, size, capacity} and **not** {begin, end}. Read
    // off its reader at RVA 0x015278D0 on 2976 (0x01730D30 on 3392, the same
    // `add edx,[rbx+8]` at +0x3C), which both list fields above call:
    // `add edx, [rbx+8]` takes the size, `mov eax, [rbx+0xC]` the capacity, and
    // the append does `lea rcx,[rax+rax*2]` then `[rax+rcx*8]`, so an element
    // is size*24 bytes along. Treating +0x08 as an end pointer is what made
    // every row on the third pass report an empty list, including ones that
    // plainly have entries: the size failed the plausible-pointer test and the
    // read was abandoned before anything was looked at.
    inline constexpr unsigned kOff_List_Items = 0x00; // void*
    inline constexpr unsigned kOff_List_Size  = 0x08; // u32
    inline constexpr unsigned kOff_List_Cap   = 0x0C; // u32

    // A UseResourceStat, 0x18 bytes. _statusInfo is a
    // StaticInfoWrapper<...,ushort>, so it names its status by the u16 row
    // index and not by the key. That is why the first pointer walk found
    // nothing: it hunted the u32 key 1000026 and the def address, and an entry
    // holds neither.
    //
    // The reader's own zero-initialised template confirms every offset:
    // byte 3 at +0x00, 0xFFFF at +0x02, 0 at +0x04, 0 at +0x08, 0xFFFFFFFF at
    // +0x10, which is the two status references at +0x10 and +0x12 set to none.
    inline constexpr unsigned kRec_UseResourceStatBytes = 0x18;
    inline constexpr unsigned kOff_URS_StatType       = 0x00; // u8
    inline constexpr unsigned kOff_URS_StatusInfo     = 0x02; // u16 row index
    inline constexpr unsigned kOff_URS_IsRegen        = 0x04; // u8
    inline constexpr unsigned kOff_URS_VaryStatAmount = 0x08; // 8 bytes, the cost
    inline constexpr unsigned kOff_URS_IncreaseStatus = 0x10; // u16
    inline constexpr unsigned kOff_URS_DecreaseStatus = 0x12; // u16

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
    // The full gallop, whose regen rate is the number that decides whether a
    // horse tires. +9000 a tick as the game ships it, against +100000 standing.
    inline constexpr const char* kSkillKey_Gallop   = "Skill_HorseMoveLv5_100";
    // The control. The extracted table says this row carries no Stamina cost,
    // so anything the walk finds on it is something every skill shares and not
    // the cost list.
    inline constexpr const char* kSkillKey_Control  = "Skill_Looting";
}
