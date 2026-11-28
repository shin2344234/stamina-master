#pragma once
#include <cstdint>

// Function hooks with absolute 64-bit jumps. MinHook needs a scratch page
// within 1 GB of the target and the game's image is surrounded by its own
// DLLs with nothing free nearby, so MH_CreateHook fails there with
// MH_ERROR_MEMORY_ALLOC. These hooks steal the first instructions (measured
// with MinHook's HDE decoder, rejected if any is rip-relative), put them in a
// trampoline anywhere in memory, and patch `mov rax, detour; jmp rax` over the
// entry. Other threads are suspended around the write.
namespace sm::farhook
{
    // Installs a detour. *original receives the trampoline (callable as the
    // original function). Returns false and fills `why` on failure.
    bool Install(const char* name, uintptr_t target, void* detour, void** original, char* why, unsigned whyLen);

    // Every general register as it stood at a mid-function hook, in the order
    // the stub pushes them. A callback may change any of them and the game
    // carries on with the new value. rsp is not here: the stub puts it back
    // itself, and a callback has no business moving it.
    struct MidRegs
    {
        uint64_t rax, rcx, rdx, rbx, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rflags;
    };

    // Hooks the middle of a function rather than its entry. The patch is
    // `jmp [rip+0]` (14 bytes), which changes no register, so it can go where
    // rax still holds something. The stub saves every general register, the
    // flags and xmm0 to xmm5, calls callback(&regs) on an aligned stack, puts
    // them back, runs the stolen instructions and jumps back. The stolen
    // instructions must be whole, at least 14 bytes, and position independent:
    // no relative branch, no call, no rip-relative operand. Nothing may jump
    // into the middle of them, which the caller checks, since only it knows
    // the function.
    bool InstallMid(const char* name, uintptr_t target, void (*callback)(MidRegs*), char* why, unsigned whyLen);

    void RemoveAll();
}
