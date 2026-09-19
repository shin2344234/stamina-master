#pragma once
#include <Windows.h>

namespace us::Mod
{
    void Initialize(HMODULE module);
    void Shutdown(bool processExiting);
}
