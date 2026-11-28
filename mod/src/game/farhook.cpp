#include "game/farhook.h"

#include "game/mem.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include <hde64.h>
}

namespace sm::farhook
{
    struct Entry { uintptr_t target; unsigned stolen; unsigned char orig[32]; };
    static Entry g_entries[96];
    static int   g_n = 0;
    static unsigned char* g_page = nullptr;
    static unsigned g_used = 0;

    static unsigned char* Alloc(unsigned n)
    {
        if (!g_page || g_used + n > 4096)
        {
            g_page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            g_used = 0;
            if (!g_page) return nullptr;
        }
        unsigned char* p = g_page + g_used;
        g_used += (n + 15) & ~15u;
        return p;
    }

    // Bytes to steal: whole instructions totalling at least 12, none of them
    // rip-relative (a jump, call or memory operand that would point elsewhere
    // once moved), none ending the function early.
    static unsigned Measure(uintptr_t target, char* why, unsigned whyLen, unsigned atLeast = 12, bool allowCall = true)
    {
        // Data can decode as instructions. Refusing a target that is not in
        // executable memory is the check that would have stopped session
        // five patching a jump over an RTTI locator in .arch.
        if (!sm::mem::Executable(target, 16))
        {
            snprintf(why, whyLen, "target is not in executable memory");
            return 0;
        }
        unsigned len = 0;
        while (len < atLeast)
        {
            hde64s hs;
            const unsigned l = hde64_disasm(reinterpret_cast<const void*>(target + len), &hs);
            if (hs.flags & F_ERROR) { snprintf(why, whyLen, "undecodable instruction at +%u", len); return 0; }
            // A `call rel32` (E8) is relocated into the trampoline as an
            // absolute call, so a prologue like `sub rsp,28h; call X` can be
            // hooked. Every other relative form is still refused, and a mid
            // hook copies its bytes verbatim, so it refuses that one too.
            if ((hs.flags & F_RELATIVE) && !(allowCall && hs.opcode == 0xE8 && l == 5))
            { snprintf(why, whyLen, "relative branch at +%u", len); return 0; }
            if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5) { snprintf(why, whyLen, "rip-relative operand at +%u", len); return 0; }
            if (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC) { snprintf(why, whyLen, "function ends before 12 bytes"); return 0; }
            len += l;
            if (len > 31) { snprintf(why, whyLen, "prologue too long"); return 0; }
        }
        return len;
    }

    // Suspends every other thread; refuses (returns false) while one of them
    // sits inside the bytes about to change, so the caller can retry.
    static int SuspendOthers(HANDLE* handles, int max, uintptr_t lo, uintptr_t hi, bool* inside)
    {
        *inside = false;
        int n = 0;
        const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        THREADENTRY32 te; te.dwSize = sizeof te;
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == me || n >= max) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) continue;
                if (SuspendThread(h) == static_cast<DWORD>(-1)) { CloseHandle(h); continue; }
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &ctx) && ctx.Rip >= lo && ctx.Rip < hi) *inside = true;
                handles[n++] = h;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        return n;
    }
    static void ResumeAll(HANDLE* handles, int n)
    {
        for (int i = 0; i < n; ++i) { ResumeThread(handles[i]); CloseHandle(handles[i]); }
    }

    // Fails rather than patch under a thread that keeps sitting inside the
    // bytes: a thread resumed halfway through a rewritten instruction crashes
    // the game, while a missing hook only costs a fallback or one feature.
    static bool WriteCode(uintptr_t dst, const void* src, unsigned n, char* why, unsigned whyLen)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(dst), n, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "VirtualProtect failed"); return false; }
        HANDLE handles[512]; bool inside = false; int cnt = 0;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            cnt = SuspendOthers(handles, 512, dst, dst + n, &inside);
            if (!inside) break;
            ResumeAll(handles, cnt);
            cnt = 0;
            Sleep(1);
        }
        if (inside)
        {
            VirtualProtect(reinterpret_cast<void*>(dst), n, old, &old);
            snprintf(why, whyLen, "a thread stayed inside the prologue");
            return false;
        }
        memcpy(reinterpret_cast<void*>(dst), src, n);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(dst), n);
        ResumeAll(handles, cnt);
        VirtualProtect(reinterpret_cast<void*>(dst), n, old, &old);
        return true;
    }

    bool Install(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen)
    {
        (void)name;
        why[0] = 0;
        if (!target) { snprintf(why, whyLen, "no target"); return false; }
        if (g_n >= static_cast<int>(sizeof g_entries / sizeof g_entries[0]))
        { snprintf(why, whyLen, "hook table full"); return false; }
        const unsigned stolen = Measure(target, why, whyLen);
        if (!stolen) return false;

        // Trampoline: the stolen bytes, then `jmp [rip+0]` to the rest of the
        // function. That form keeps every register, which matters for entries
        // like `mov rax, rsp` whose value the function still needs. A stolen
        // `call rel32` becomes `mov rax, abs; call rax` (12 bytes for 5), which
        // is fine because rax is volatile at a call boundary and the callee
        // returns into the trampoline, which then carries on.
        unsigned char* tramp = Alloc(stolen + 14 + 3 * 7);
        if (!tramp) { snprintf(why, whyLen, "trampoline page allocation failed"); return false; }
        unsigned char* p = tramp;
        for (unsigned off = 0; off < stolen;)
        {
            hde64s hs;
            const unsigned l = hde64_disasm(reinterpret_cast<const void*>(target + off), &hs);
            if (hs.opcode == 0xE8 && l == 5)
            {
                const uintptr_t callee = target + off + 5 + static_cast<int32_t>(hs.imm.imm32);
                p[0] = 0x48; p[1] = 0xB8; memcpy(p + 2, &callee, 8); p[10] = 0xFF; p[11] = 0xD0;
                p += 12;
            }
            else
            {
                memcpy(p, reinterpret_cast<const void*>(target + off), l);
                p += l;
            }
            off += l;
        }
        p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
        const uintptr_t back = target + stolen;
        memcpy(p + 6, &back, 8);

        // Entry patch: `mov rax, detour; jmp rax`, padded with int3. rax is
        // volatile and carries nothing at a call boundary.
        unsigned char patch[32];
        memset(patch, 0xCC, sizeof patch);
        patch[0] = 0x48; patch[1] = 0xB8;
        memcpy(patch + 2, &detour, 8);
        patch[10] = 0xFF; patch[11] = 0xE0;

        Entry& e = g_entries[g_n];
        e.target = target; e.stolen = stolen;
        memcpy(e.orig, reinterpret_cast<const void*>(target), stolen);

        // Publish the trampoline before the entry patch, never after.
        //
        // The patch is what sends callers to the detour, and every detour calls
        // its original straight away: hkMove opens with oMove(...) and nothing
        // checks it first. Writing the patch first leaves a window, a few
        // instructions wide, in which a game thread already inside the process
        // can enter the detour and call through a null pointer. The movement
        // tick runs thousands of times a second and these hooks go in during
        // world load, so the window is small but it is aimed at a firehose.
        //
        // Publishing early is safe in a way publishing late is not: the
        // trampoline is complete here, and until the patch lands nothing can
        // reach the detour that would use it.
        *original = tramp;
        if (!WriteCode(target, patch, stolen, why, whyLen)) { *original = nullptr; return false; }
        ++g_n;
        return true;
    }

    bool InstallMid(const char* name, uintptr_t target, void (*callback)(MidRegs*), char* why, unsigned whyLen)
    {
        (void)name;
        why[0] = 0;
        if (!target || !callback) { snprintf(why, whyLen, "no target"); return false; }
        if (g_n >= static_cast<int>(sizeof g_entries / sizeof g_entries[0]))
        { snprintf(why, whyLen, "hook table full"); return false; }
        const unsigned stolen = Measure(target, why, whyLen, 14, false);
        if (!stolen) return false;

        static const unsigned char kSave[] = {
            0x9C,                                     // pushfq
            0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,   // push r15 r14 r13 r12
            0x41, 0x53, 0x41, 0x52, 0x41, 0x51, 0x41, 0x50,   // push r11 r10 r9 r8
            0x57, 0x56, 0x55, 0x53, 0x52, 0x51, 0x50,         // push rdi rsi rbp rbx rdx rcx rax
            0x48, 0x89, 0xE1,                         // mov rcx, rsp      (MidRegs*)
            0x48, 0x89, 0xE3,                         // mov rbx, rsp      (callee keeps rbx)
            0x48, 0x83, 0xE4, 0xF0,                   // and rsp, -16
            0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, // sub rsp, 0x80     (shadow + xmm0-5)
            0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20,       // movdqu [rsp+0x20], xmm0
            0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,       // movdqu [rsp+0x30], xmm1
            0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40,       // movdqu [rsp+0x40], xmm2
            0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50,       // movdqu [rsp+0x50], xmm3
            0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x60,       // movdqu [rsp+0x60], xmm4
            0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x70,       // movdqu [rsp+0x70], xmm5
            0x48, 0xB8,                               // mov rax, imm64    (callback follows)
        };
        static const unsigned char kRestore[] = {
            0xFF, 0xD0,                               // call rax
            0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20,       // movdqu xmm0, [rsp+0x20]
            0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,       // movdqu xmm1, [rsp+0x30]
            0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40,       // movdqu xmm2, [rsp+0x40]
            0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,       // movdqu xmm3, [rsp+0x50]
            0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x60,       // movdqu xmm4, [rsp+0x60]
            0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x70,       // movdqu xmm5, [rsp+0x70]
            0x48, 0x89, 0xDC,                         // mov rsp, rbx
            0x58, 0x59, 0x5A, 0x5B, 0x5D, 0x5E, 0x5F,         // pop rax rcx rdx rbx rbp rsi rdi
            0x41, 0x58, 0x41, 0x59, 0x41, 0x5A, 0x41, 0x5B,   // pop r8 r9 r10 r11
            0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F,   // pop r12 r13 r14 r15
            0x9D,                                     // popfq
        };
        static_assert(sizeof(MidRegs) == 16 * 8, "MidRegs must match the push order in kSave");

        const unsigned total = sizeof kSave + 8 + sizeof kRestore + stolen + 14;
        unsigned char* stub = Alloc(total);
        if (!stub) { snprintf(why, whyLen, "stub page allocation failed"); return false; }
        unsigned char* p = stub;
        memcpy(p, kSave, sizeof kSave); p += sizeof kSave;
        const uintptr_t cb = reinterpret_cast<uintptr_t>(callback);
        memcpy(p, &cb, 8); p += 8;
        memcpy(p, kRestore, sizeof kRestore); p += sizeof kRestore;
        memcpy(p, reinterpret_cast<const void*>(target), stolen); p += stolen;
        p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
        const uintptr_t back = target + stolen;
        memcpy(p + 6, &back, 8);

        // The patch: `jmp [rip+0]` and the stub's address, int3 after.
        unsigned char patch[32];
        memset(patch, 0xCC, sizeof patch);
        patch[0] = 0xFF; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0;
        const uintptr_t to = reinterpret_cast<uintptr_t>(stub);
        memcpy(patch + 6, &to, 8);

        Entry& e = g_entries[g_n];
        e.target = target; e.stolen = stolen;
        memcpy(e.orig, reinterpret_cast<const void*>(target), stolen);
        if (!WriteCode(target, patch, stolen, why, whyLen)) return false;
        ++g_n;
        return true;
    }

    void RemoveAll()
    {
        char why[64];
        for (int i = g_n - 1; i >= 0; --i) WriteCode(g_entries[i].target, g_entries[i].orig, g_entries[i].stolen, why, sizeof why);
        g_n = 0;
        // The trampoline page stays: a game thread may still be running through it.
    }
}
