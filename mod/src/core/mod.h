#pragma once
#include <Windows.h>

namespace sm::Mod
{
    void Initialize(HMODULE module);
    void Shutdown(bool processExiting);
}
