#include "hook_transaction.hpp"

#include <MinHook.h>

#include <cstdint>
#include <cstring>

#include <windows.h>

namespace cinemania
{
namespace
{
struct PatchResult
{
    unsigned long code = 0;
    DWORD old_protection = 0;
    bool exchanged = false;
    bool protected_again = true;
};

PatchResult exchange_import(const ImportHook& hook, void* expected, void* replacement)
{
    PatchResult result{};
    if (!VirtualProtect(hook.slot, sizeof(void*), PAGE_READWRITE, &result.old_protection))
    {
        result.code = GetLastError();
        return result;
    }

    void* previous = InterlockedCompareExchangePointer(hook.slot, replacement, expected);
    result.exchanged = previous == expected;
    DWORD ignored = 0;
    result.protected_again =
        VirtualProtect(hook.slot, sizeof(void*), result.old_protection, &ignored) != 0;
    if (!result.protected_again)
    {
        result.code = GetLastError();
    }
    else if (!result.exchanged)
    {
        result.code = ERROR_INVALID_DATA;
    }
    return result;
}

bool restore_import_protection(const ImportHook& hook, const PatchResult& patch)
{
    if (patch.protected_again)
    {
        return true;
    }

    DWORD ignored = 0;
    return VirtualProtect(hook.slot, sizeof(void*), patch.old_protection, &ignored) != 0;
}

bool rollback_imports(std::span<ImportHook> imports, std::size_t count)
{
    bool clean = true;
    while (count > 0)
    {
        --count;
        const PatchResult patch =
            exchange_import(imports[count], imports[count].replacement, imports[count].expected);
        const bool protected_again = restore_import_protection(imports[count], patch);
        if (!patch.exchanged || !protected_again)
        {
            clean = false;
        }
    }
    return clean;
}

bool discard_prepared(std::span<EntryHook> entries, std::size_t count)
{
    bool clean = true;
    while (count > 0)
    {
        --count;
        if (MH_RemoveHook(entries[count].target) != MH_OK)
        {
            clean = false;
        }
    }
    if (clean && MH_Uninitialize() != MH_OK)
    {
        clean = false;
    }
    return clean;
}

bool disable_entries(std::span<EntryHook> entries, std::size_t count)
{
    // MinHook restores bytes without comparing them. Do not overwrite a later foreign patch.
    // A concurrent unsynchronized writer can still race this comparison and MinHook's write.
    for (std::size_t index = 0; index < count; ++index)
    {
        const EntryHook& hook = entries[index];
        if (hook.expected_bytes == nullptr || hook.expected_size < 5)
        {
            return false;
        }
        const auto* target = static_cast<const unsigned char*>(hook.target);
        const std::uint32_t displacement =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hook.detour) -
                                       reinterpret_cast<std::uintptr_t>(hook.target) - 5);
        unsigned char jump[5]{0xe9};
        std::memcpy(jump + 1, &displacement, sizeof(displacement));
        const bool original = std::memcmp(target, hook.expected_bytes, hook.expected_size) == 0;
        const bool owned = std::memcmp(target, jump, sizeof(jump)) == 0 &&
                           std::memcmp(target + sizeof(jump),
                                       hook.expected_bytes + sizeof(jump),
                                       hook.expected_size - sizeof(jump)) == 0;
        if (!original && !owned)
        {
            return false;
        }
    }

    bool clean = true;
    for (std::size_t index = 0; index < count; ++index)
    {
        const MH_STATUS status = MH_QueueDisableHook(entries[index].target);
        if (status != MH_OK && status != MH_ERROR_DISABLED)
        {
            clean = false;
        }
    }
    if (!clean)
    {
        return false;
    }
    return MH_ApplyQueued() == MH_OK;
}
} // namespace

HookResult install_hooks(std::span<EntryHook> entries, std::span<ImportHook> imports)
{
    HookResult result{};
    result.domain = HookErrorDomain::minhook;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        result.stage = "minhook initialize";
        result.code = static_cast<unsigned long>(status);
        return result;
    }

    std::size_t created = 0;
    for (EntryHook& hook : entries)
    {
        status = MH_CreateHook(hook.target, hook.detour, hook.original);
        if (status != MH_OK)
        {
            result.stage = "minhook create";
            result.code = static_cast<unsigned long>(status);
            result.residual = !discard_prepared(entries, created);
            return result;
        }
        ++created;
    }

    for (const EntryHook& hook : entries)
    {
        if (hook.expected_bytes != nullptr &&
            std::memcmp(hook.target, hook.expected_bytes, hook.expected_size) != 0)
        {
            result.domain = HookErrorDomain::validation;
            result.stage = "entry revalidation";
            result.residual = !discard_prepared(entries, created);
            return result;
        }
    }

    std::size_t patched = 0;
    for (const ImportHook& hook : imports)
    {
        const PatchResult patch = exchange_import(hook, hook.expected, hook.replacement);
        if (patch.exchanged)
        {
            ++patched;
        }
        if (!patch.exchanged || !patch.protected_again)
        {
            result.domain = HookErrorDomain::win32;
            result.stage = "import exchange";
            result.code = patch.code;
            const bool restored = rollback_imports(imports, patched);
            // A rollback exchange may see the temporary writable protection. Restore the
            // protection observed before the failed forward exchange, even on a CAS mismatch.
            const bool protected_again = restore_import_protection(hook, patch);
            const bool discarded = discard_prepared(entries, created);
            result.residual = !protected_again || !restored || !discarded;
            return result;
        }
    }

    for (const EntryHook& hook : entries)
    {
        status = MH_QueueEnableHook(hook.target);
        if (status != MH_OK)
        {
            result.domain = HookErrorDomain::minhook;
            result.stage = "minhook queue enable";
            result.code = static_cast<unsigned long>(status);
            const bool restored = rollback_imports(imports, patched);
            const bool discarded = discard_prepared(entries, created);
            result.residual = !restored || !discarded;
            return result;
        }
    }

    for (const EntryHook& hook : entries)
    {
        if (hook.expected_bytes != nullptr &&
            std::memcmp(hook.target, hook.expected_bytes, hook.expected_size) != 0)
        {
            result.domain = HookErrorDomain::validation;
            result.stage = "entry activation validation";
            const bool restored = rollback_imports(imports, patched);
            const bool discarded = discard_prepared(entries, created);
            result.residual = !restored || !discarded;
            return result;
        }
    }

    result.domain = HookErrorDomain::minhook;
    status = MH_ApplyQueued();
    if (status != MH_OK)
    {
        result.stage = "minhook apply";
        result.code = static_cast<unsigned long>(status);
        // MinHook may have moved an in-flight thread into a trampoline before failing.
        // Disabling patches cannot prove those trampolines unreachable, so retain them and
        // MinHook state for the lifetime of the pinned DLL.
        const bool disabled = disable_entries(entries, created);
        const bool restored = rollback_imports(imports, patched);
        result.residual = !disabled || !restored;
        return result;
    }

    result.domain = HookErrorDomain::none;
    result.stage = "installed";
    result.installed = static_cast<unsigned int>(entries.size() + imports.size());
    result.success = true;
    return result;
}
} // namespace cinemania
