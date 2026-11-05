// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <cstdio>
#include <windows.h>
inline void InstallExceptionDiagnostics() {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
        HMODULE module = nullptr;
        wchar_t path[32768]{};
        auto print = [&](void *address) {
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module);
            GetModuleFileNameW(module, path, 32768);
            fprintf(stderr, "  %ls + %llx\n", path, (unsigned long long)((uintptr_t)address - (uintptr_t)module));
        };
        fprintf(stderr, "Exception %08lx\n", e->ExceptionRecord->ExceptionCode);
        print(e->ExceptionRecord->ExceptionAddress);
        void *frames[32]{};
        const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
            print(frames[i]);
        return EXCEPTION_EXECUTE_HANDLER;
    });
}
