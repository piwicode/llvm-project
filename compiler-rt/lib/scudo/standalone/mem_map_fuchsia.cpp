//===-- mem_map_fuchsia.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mem_map_fuchsia.h"

#include "common.h"
#include "string_utils.h"

#if SCUDO_FUCHSIA

#include <zircon/process.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

namespace scudo {

static void NORETURN dieOnError(zx_status_t Status, const char *FnName,
                                uptr Size) {
  ScopedString Error;
  Error.append("SCUDO ERROR: %s failed with size %zuKB (%s)", FnName,
               Size >> 10, _zx_status_get_string(Status));
  outputRaw(Error.data());
  die();
}

static void setVmoName(zx_handle_t Vmo, const char *Name) {
  size_t Len = strlen(Name);
  DCHECK_LT(Len, ZX_MAX_NAME_LEN);
  zx_status_t Status = _zx_object_set_property(Vmo, ZX_PROP_NAME, Name, Len);
  CHECK_EQ(Status, ZX_OK);
}

// Checks if MAP_ALLOWNOMEM allows the given error code.
static bool IsNoMemError(zx_status_t Status) {
  // Note: _zx_vmar_map returns ZX_ERR_NO_RESOURCES if the VMAR does not contain
  // a suitable free spot.
  return Status == ZX_ERR_NO_MEMORY || Status == ZX_ERR_NO_RESOURCES;
}

static uptr mapNewVmo(uptr Size, const char *Name, zx_vm_option_t MapFlags,
                      bool AllowNoMem) {
  zx_handle_t Vmo = ZX_HANDLE_INVALID;
  zx_status_t Status = _zx_vmo_create(Size, 0, &Vmo);
  if (UNLIKELY(Status != ZX_OK)) {
    if (AllowNoMem && IsNoMemError(Status))
      return 0;
    dieOnError(Status, "zx_vmo_create", Size);
  }
  if (Name != nullptr)
    setVmoName(Vmo, Name);

  uptr Base = 0;
  Status = _zx_vmar_map(_zx_vmar_root_self(), MapFlags, 0, Vmo, 0, Size, &Base);
  const zx_status_t CloseStatus = _zx_handle_close(Vmo);
  CHECK_EQ(CloseStatus, ZX_OK);
  if (UNLIKELY(Status != ZX_OK)) {
    if (AllowNoMem && IsNoMemError(Status))
      return 0;
    dieOnError(Status, "zx_vmar_map", Size);
  }
  DCHECK_NE(Base, 0U);
  return Base;
}

// Note: this constructor is only called by ReservedMemoryFuchsia::dispatch.
MemMapFuchsia::MemMapFuchsia(uptr Base, uptr Capacity, bool OwnsVmo)
    : MapBase(Base), MapCapacity(Capacity), OwnsVmo(OwnsVmo) {}

bool MemMapFuchsia::mapImpl(UNUSED uptr Addr, uptr Size, const char *Name,
                            uptr Flags) {
  const bool AllowNoMem = !!(Flags & MAP_ALLOWNOMEM);
  const bool PreCommit = !!(Flags & MAP_PRECOMMIT);
  const bool NoAccess = !!(Flags & MAP_NOACCESS);

  zx_vm_option_t MapFlags = 0;
  if (!NoAccess)
    MapFlags |= ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
  const uptr NewBase = mapNewVmo(Size, Name, MapFlags, AllowNoMem);
  if (UNLIKELY(NewBase == 0))
    return false;

  if (PreCommit) {
    zx_status_t Status = _zx_vmar_op_range(
        _zx_vmar_root_self(), ZX_VMAR_OP_COMMIT, NewBase, Size, nullptr, 0);
    if (UNLIKELY(Status != ZX_OK)) {
      if (AllowNoMem && IsNoMemError(Status)) {
        Status = _zx_vmar_unmap(_zx_vmar_root_self(), NewBase, Size);
        CHECK_EQ(Status, ZX_OK);
        return false;
      }
      dieOnError(Status, "zx_vmar_op_range", Size);
    }
  }

  MapBase = NewBase;
  MapCapacity = Size;
  OwnsVmo = true;
  return true;
}

void MemMapFuchsia::unmapImpl(uptr Addr, uptr Size) {
  // Decommit before unmapping unless this removes the sole remaining mapping of
  // an exclusively owned VMO, so physical pages are freed immediately rather
  // than kept committed by surviving mappings.
  if (!OwnsVmo || Size < MapCapacity)
    releaseAndZeroPagesToOSImpl(Addr, Size);

  zx_status_t Status = _zx_vmar_unmap(_zx_vmar_root_self(), Addr, Size);
  CHECK_EQ(Status, ZX_OK);

  if (Size == MapCapacity) {
    MapBase = MapCapacity = 0;
    OwnsVmo = false;
  } else {
    if (Addr == MapBase)
      MapBase += Size;
    MapCapacity -= Size;
  }
}

bool MemMapFuchsia::remapImpl(uptr Addr, uptr Size, UNUSED const char *Name,
                              uptr Flags) {
  const bool AllowNoMem = !!(Flags & MAP_ALLOWNOMEM);
  const bool PreCommit = !!(Flags & MAP_PRECOMMIT);
  const bool NoAccess = !!(Flags & MAP_NOACCESS);

  if (PreCommit) {
    // Commit before widening permissions so an OOM rollback only needs
    // decommit (which never allocates) and never leaves the range writable.
    zx_status_t Status = _zx_vmar_op_range(
        _zx_vmar_root_self(), ZX_VMAR_OP_COMMIT, Addr, Size, nullptr, 0);
    if (UNLIKELY(Status != ZX_OK)) {
      if (AllowNoMem && IsNoMemError(Status)) {
        releaseAndZeroPagesToOSImpl(Addr, Size);
        return false;
      }
      dieOnError(Status, "zx_vmar_op_range", Size);
    }
  }

  zx_vm_option_t ProtFlags = 0;
  if (!NoAccess)
    ProtFlags |= ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
  zx_status_t Status =
      _zx_vmar_protect(_zx_vmar_root_self(), ProtFlags, Addr, Size);
  if (UNLIKELY(Status != ZX_OK)) {
    if (PreCommit)
      releaseAndZeroPagesToOSImpl(Addr, Size);
    if (AllowNoMem && IsNoMemError(Status))
      return false;
    dieOnError(Status, "zx_vmar_protect", Size);
  }

  if (PreCommit && !NoAccess) {
    // Best-effort PTE population now that permissions are active.
    zx_status_t MapStatus = _zx_vmar_op_range(
        _zx_vmar_root_self(), ZX_VMAR_OP_MAP_RANGE, Addr, Size, nullptr, 0);
    CHECK(MapStatus == ZX_OK || IsNoMemError(MapStatus));
  }

  return true;
}

void MemMapFuchsia::releaseAndZeroPagesToOSImpl(uptr From, uptr Size) {
  if (UNLIKELY(Size == 0))
    return;
  zx_status_t Status = _zx_vmar_op_range(
      _zx_vmar_root_self(), ZX_VMAR_OP_DECOMMIT, From, Size, nullptr, 0);
  CHECK_EQ(Status, ZX_OK);
}

// Not supported by default
s64 MemMapFuchsia::getResidentPagesImpl(UNUSED uptr From, UNUSED uptr Size) {
  return -1;
}

void MemMapFuchsia::setMemoryPermissionImpl(uptr Addr, uptr Size, uptr Flags) {
  const bool NoAccess = !!(Flags & MAP_NOACCESS);

  zx_vm_option_t MapFlags = 0;
  if (!NoAccess)
    MapFlags |= ZX_VM_PERM_READ | ZX_VM_PERM_WRITE;
  zx_status_t Status =
      _zx_vmar_protect(_zx_vmar_root_self(), MapFlags, Addr, Size);
  CHECK_EQ(Status, ZX_OK);
}

bool ReservedMemoryFuchsia::createImpl(UNUSED uptr Addr, uptr Size,
                                       const char *Name, uptr Flags) {
  const bool AllowNoMem = !!(Flags & MAP_ALLOWNOMEM);

  // Map a single sparse VMO once into the root VMAR with no permissions (---).
  // Because both the VMO and root VMAR have READ|WRITE rights, Zircon records
  // CAN_MAP_READ|CAN_MAP_WRITE on the VmMapping, letting remapImpl() toggle
  // subranges via zx_vmar_protect() while gaps stay --- and fault.
  // Align >= 2 MiB multiples (e.g. Primary64 reservations) to 2 MiB to keep
  // size-class regions from straddling L2 page-table boundaries; skip RISC-V 64
  // Sv39 where 2 MiB alignment would cut ASLR entropy from 26 to 17 bits.
  zx_vm_option_t MapFlags = 0;
  constexpr uptr TwoMiB = static_cast<uptr>(2) << 20;
  if (!SCUDO_RISCV64 && Size >= TwoMiB && isAligned(Size, TwoMiB))
    MapFlags |= ZX_VM_ALIGN_2MB;

  // Secondary passes Name == nullptr here and "scudo:secondary" to remapImpl()
  // (which ignores Name because the VMO handle is already closed); Primary64
  // passes "scudo:primary_reserve". Default nullptr to "scudo:secondary".
  const uptr NewBase = mapNewVmo(
      Size, Name != nullptr ? Name : "scudo:secondary", MapFlags, AllowNoMem);
  if (UNLIKELY(NewBase == 0))
    return false;

  Base = NewBase;
  Capacity = Size;
  return true;
}

void ReservedMemoryFuchsia::releaseImpl() {
  zx_status_t Status = _zx_vmar_unmap(_zx_vmar_root_self(), Base, Capacity);
  CHECK_EQ(Status, ZX_OK);
  Base = Capacity = 0;
}

ReservedMemoryFuchsia::MemMapT ReservedMemoryFuchsia::dispatchImpl(uptr Addr,
                                                                   uptr Size) {
  // A dispatch covering the entire reservation (e.g. Secondary) leaves no
  // sibling subranges, so the returned MemMap exclusively owns the backing VMO.
  const bool OwnsVmo = (Addr == Base && Size == Capacity);
  return ReservedMemoryFuchsia::MemMapT(Addr, Size, OwnsVmo);
}

} // namespace scudo

#endif // SCUDO_FUCHSIA
