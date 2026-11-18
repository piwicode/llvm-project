//===-- map_test.cpp --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "tests/scudo_unit_test.h"

#include "common.h"
#include "mem_map.h"

#include <algorithm>
#include <string.h>
#include <unistd.h>

#if SCUDO_LINUX
#include <sys/mman.h>
#endif

#if SCUDO_FUCHSIA
#include <vector>
#include <zircon/process.h>
#include <zircon/syscalls.h>
#include <zircon/syscalls/object.h>
#endif

static const char *MappingName = "scudo:test";

TEST(ScudoMapTest, PageSize) {
  EXPECT_EQ(scudo::getPageSizeCached(),
            static_cast<scudo::uptr>(sysconf(_SC_PAGESIZE)));
}

TEST(ScudoMapTest, VerifyGetResidentPages) {
  if (!SCUDO_LINUX)
    TEST_SKIP("Only valid on linux systems.");

  constexpr scudo::uptr NumPages = 512;
  const scudo::uptr SizeBytes = NumPages * scudo::getPageSizeCached();

  scudo::MemMapT MemMap;
  ASSERT_TRUE(MemMap.map(/*Addr=*/0U, SizeBytes, "ResidentMemorySize"));
  ASSERT_NE(MemMap.getBase(), 0U);

  // Only android seems to properly detect when single pages are touched.
#if SCUDO_ANDROID
  // Verify nothing should be mapped in right after the map is created.
  EXPECT_EQ(0U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Touch a page.
  scudo::u8 *Data = reinterpret_cast<scudo::u8 *>(MemMap.getBase());
  Data[0] = 1;
  EXPECT_EQ(1U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Touch a non-consective page.
  Data[scudo::getPageSizeCached() * 2] = 1;
  EXPECT_EQ(2U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Touch a page far enough that the function has to make multiple calls
  // to mincore.
  Data[scudo::getPageSizeCached() * 300] = 1;
  EXPECT_EQ(3U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Touch another page in the same range to make sure the second
  // read is working.
  Data[scudo::getPageSizeCached() * 400] = 1;
  EXPECT_EQ(4U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));
#endif

  // Now write the whole thing.
  memset(reinterpret_cast<void *>(MemMap.getBase()), 1, SizeBytes);
  scudo::s64 ResidentPages =
      MemMap.getResidentPages(MemMap.getBase(), SizeBytes);
  EXPECT_EQ(NumPages, static_cast<uintptr_t>(ResidentPages));

  MemMap.unmap();
}

TEST(ScudoMapTest, VerifyReleasePagesToOS) {
  if (!SCUDO_LINUX)
    TEST_SKIP("Only valid on linux systems.");

  constexpr scudo::uptr NumPages = 1000;
  const scudo::uptr SizeBytes = NumPages * scudo::getPageSizeCached();

  scudo::MemMapT MemMap;
  ASSERT_TRUE(MemMap.map(/*Addr=*/0U, SizeBytes, "ResidentMemorySize"));
  ASSERT_NE(MemMap.getBase(), 0U);

  void *P = reinterpret_cast<void *>(MemMap.getBase());
  EXPECT_EQ(0U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Make the entire map resident.
  memset(P, 1, SizeBytes);
  scudo::s64 ResidentPages =
      MemMap.getResidentPages(MemMap.getBase(), SizeBytes);
  if (ResidentPages >= 0)
    EXPECT_EQ(NumPages, static_cast<uintptr_t>(ResidentPages));

  // Should release the memory to the kernel immediately.
  MemMap.releasePagesToOS(MemMap.getBase(), SizeBytes);
  EXPECT_EQ(0U, MemMap.getResidentPages(MemMap.getBase(), SizeBytes));

  // Make the entire map resident again.
  memset(P, 1, SizeBytes);
  ResidentPages = MemMap.getResidentPages(MemMap.getBase(), SizeBytes);
  EXPECT_EQ(NumPages, static_cast<uintptr_t>(ResidentPages));

  MemMap.unmap();
}

TEST(ScudoMapTest, Zeros) {
  const scudo::uptr Size = 1ull << 20;

  scudo::MemMapT MemMap;
  ASSERT_TRUE(MemMap.map(/*Addr=*/0U, Size, "Zeros"));
  ASSERT_NE(MemMap.getBase(), 0U);
  scudo::uptr *P = reinterpret_cast<scudo::uptr *>(MemMap.getBase());
  const ptrdiff_t N = Size / sizeof(scudo::uptr);
  EXPECT_EQ(std::count(P, P + N, 0), N);

  memset(P, 1, Size);
  EXPECT_EQ(std::count(P, P + N, 0), 0);

  MemMap.releasePagesToOS(MemMap.getBase(), Size);
  EXPECT_EQ(std::count(P, P + N, 0), N);

  MemMap.unmap();
}

TEST(ScudoMapDeathTest, MapNoAccessUnmap) {
  const scudo::uptr Size = 4 * scudo::getPageSizeCached();
  scudo::ReservedMemoryT ReservedMemory;

  ASSERT_TRUE(ReservedMemory.create(/*Addr=*/0U, Size, MappingName));
  EXPECT_NE(ReservedMemory.getBase(), 0U);
  SCUDO_EXPECT_DEATH(
      memset(reinterpret_cast<void *>(ReservedMemory.getBase()), 0xaa, Size),
      "");

  ReservedMemory.release();
}

TEST(ScudoMapDeathTest, MapUnmap) {
  const scudo::uptr Size = 4 * scudo::getPageSizeCached();
  SCUDO_EXPECT_DEATH(
      {
        // Repeat few time to avoid missing crash if it's mmaped by unrelated
        // code.
        for (int i = 0; i < 10; ++i) {
          scudo::MemMapT MemMap;
          MemMap.map(/*Addr=*/0U, Size, MappingName);
          scudo::uptr P = MemMap.getBase();
          if (P == 0U)
            continue;
          MemMap.unmap();
          memset(reinterpret_cast<void *>(P), 0xbb, Size);
        }
      },
      "");
}

TEST(ScudoMapDeathTest, MapWithGuardUnmap) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 4 * PageSize;
  scudo::ReservedMemoryT ReservedMemory;
  ASSERT_TRUE(
      ReservedMemory.create(/*Addr=*/0U, Size + 2 * PageSize, MappingName));
  ASSERT_NE(ReservedMemory.getBase(), 0U);

  scudo::MemMapT MemMap =
      ReservedMemory.dispatch(ReservedMemory.getBase(), Size + 2 * PageSize);
  ASSERT_TRUE(MemMap.isAllocated());
  scudo::uptr Q = MemMap.getBase() + PageSize;
  ASSERT_TRUE(MemMap.remap(Q, Size, MappingName));
  memset(reinterpret_cast<void *>(Q), 0xaa, Size);
  SCUDO_EXPECT_DEATH(memset(reinterpret_cast<void *>(Q), 0xaa, Size + 1), "");
  MemMap.unmap();
}

// These death tests only fail when debugging is enabled.
#if SCUDO_LINUX
TEST(ScudoMapDeathTest, ResidentPagesNotMapped) {
  scudo::MemMapT MemMap;
  ASSERT_EQ(MemMap.getResidentPages(), 0);
}
#endif

TEST(ScudoMapTest, MapGrowUnmap) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 4 * PageSize;
  scudo::ReservedMemoryT ReservedMemory;
  ReservedMemory.create(/*Addr=*/0U, Size, MappingName);
  ASSERT_TRUE(ReservedMemory.isCreated());

  scudo::MemMapT MemMap =
      ReservedMemory.dispatch(ReservedMemory.getBase(), Size);
  ASSERT_TRUE(MemMap.isAllocated());
  scudo::uptr Q = MemMap.getBase() + PageSize;
  ASSERT_TRUE(MemMap.remap(Q, PageSize, MappingName));
  memset(reinterpret_cast<void *>(Q), 0xaa, PageSize);
  Q += PageSize;
  ASSERT_TRUE(MemMap.remap(Q, PageSize, MappingName));
  memset(reinterpret_cast<void *>(Q), 0xbb, PageSize);
  MemMap.unmap();
}

// Verify that zeroing works properly.
TEST(ScudoMapTest, Zeroing) {
  scudo::ReservedMemoryT ReservedMemory;
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  const scudo::uptr Size = 3 * PageSize;
  ReservedMemory.create(/*Addr=*/0U, Size, MappingName);
  ASSERT_TRUE(ReservedMemory.isCreated());

  scudo::MemMapT MemMap = ReservedMemory.dispatch(ReservedMemory.getBase(),
                                                  ReservedMemory.getCapacity());
  EXPECT_TRUE(
      MemMap.remap(MemMap.getBase(), MemMap.getCapacity(), MappingName));
  unsigned char *Data = reinterpret_cast<unsigned char *>(MemMap.getBase());
  memset(Data, 1U, MemMap.getCapacity());
  // Spot check some values.
  EXPECT_EQ(1U, Data[0]);
  EXPECT_EQ(1U, Data[PageSize]);
  EXPECT_EQ(1U, Data[PageSize * 2]);
  MemMap.releaseAndZeroPagesToOS(MemMap.getBase(), MemMap.getCapacity());
  EXPECT_EQ(0U, Data[0]);
  EXPECT_EQ(0U, Data[PageSize]);
  EXPECT_EQ(0U, Data[PageSize * 2]);

#if SCUDO_LINUX
  // Now verify that if madvise fails, the data is still zeroed.
  memset(Data, 1U, MemMap.getCapacity());
  if (mlock(Data, MemMap.getCapacity()) != -1) {
    EXPECT_EQ(1U, Data[0]);
    EXPECT_EQ(1U, Data[PageSize]);
    EXPECT_EQ(1U, Data[PageSize * 2]);
    MemMap.releaseAndZeroPagesToOS(MemMap.getBase(), MemMap.getCapacity());
    EXPECT_EQ(0U, Data[0]);
    EXPECT_EQ(0U, Data[PageSize]);
    EXPECT_EQ(0U, Data[PageSize * 2]);

    EXPECT_NE(-1, munlock(Data, MemMap.getCapacity()));
  }
#endif

  MemMap.unmap();
}

// Verify that Fuchsia kernel resources (VMOs, open handles, and committed
// pages) are released as expected across reservation, remap, partial/shared
// unmap, and OOM paths.
#if SCUDO_FUCHSIA
namespace {

struct VmoSummary {
  size_t Count = 0;
  size_t ViaHandleCount = 0;
  uint64_t CommittedBytes = 0;
};

struct MapSummary {
  size_t RwMappings = 0;
  size_t NoAccessMappings = 0;
  size_t Vmars = 0;
};

template <typename InfoT>
void queryProcessInfo(zx_object_info_topic_t Topic, std::vector<InfoT> &Out) {
  size_t Cap = 256;
  for (;;) {
    Out.resize(Cap);
    size_t Actual = 0, Avail = 0;
    ASSERT_EQ(zx_object_get_info(zx_process_self(), Topic, Out.data(),
                                 Out.size() * sizeof(InfoT), &Actual, &Avail),
              ZX_OK);
    if (Actual == Avail) {
      Out.resize(Actual);
      return;
    }
    Cap = Avail + 64;
  }
}

void summarizeVmosByName(const char *Name, VmoSummary &Out) {
  Out = {};
  std::vector<zx_info_vmo_t> Vmos;
  ASSERT_NO_FATAL_FAILURE(queryProcessInfo(ZX_INFO_PROCESS_VMOS, Vmos));
  std::vector<zx_koid_t> Seen;
  for (const zx_info_vmo_t &V : Vmos) {
    if (strcmp(V.name, Name) != 0)
      continue;
    if (V.flags & ZX_INFO_VMO_VIA_HANDLE)
      ++Out.ViaHandleCount;
    if (std::find(Seen.begin(), Seen.end(), V.koid) == Seen.end()) {
      Seen.push_back(V.koid);
      ++Out.Count;
      Out.CommittedBytes += V.committed_bytes;
    }
  }
}

void summarizeVmoByKoid(zx_koid_t Koid, VmoSummary &Out) {
  Out = {};
  std::vector<zx_info_vmo_t> Vmos;
  ASSERT_NO_FATAL_FAILURE(queryProcessInfo(ZX_INFO_PROCESS_VMOS, Vmos));
  bool Found = false;
  for (const zx_info_vmo_t &V : Vmos) {
    if (V.koid != Koid)
      continue;
    if (V.flags & ZX_INFO_VMO_VIA_HANDLE)
      ++Out.ViaHandleCount;
    if (!Found) {
      Found = true;
      Out.Count = 1;
      Out.CommittedBytes = V.committed_bytes;
    }
  }
}

void countMapsInRange(scudo::uptr Base, scudo::uptr Size, MapSummary &Out) {
  Out = {};
  std::vector<zx_info_maps_t> Maps;
  ASSERT_NO_FATAL_FAILURE(queryProcessInfo(ZX_INFO_PROCESS_MAPS, Maps));
  const scudo::uptr End = Base + Size;
  for (const zx_info_maps_t &M : Maps) {
    if (M.base < Base || M.base + M.size > End)
      continue;
    if (M.type == ZX_INFO_MAPS_TYPE_MAPPING) {
      if (M.u.mapping.mmu_flags == (ZX_VM_PERM_READ | ZX_VM_PERM_WRITE))
        ++Out.RwMappings;
      else if (M.u.mapping.mmu_flags == 0)
        ++Out.NoAccessMappings;
    } else if (M.type == ZX_INFO_MAPS_TYPE_VMAR) {
      ++Out.Vmars;
    }
  }
}

} // namespace

TEST(ScudoMapTest, FuchsiaPrimarySingleVmoZeroHandlesAndDecommit) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  constexpr scudo::uptr NumRegions = 45;
  const scudo::uptr RegionSize = 4 * PageSize;
  const scudo::uptr TotalSize = NumRegions * RegionSize;

  scudo::ReservedMemoryT R;
  ASSERT_TRUE(R.create(/*Addr=*/0U, TotalSize, "scudo:ut_pri"));
  const scudo::uptr Base = R.getBase();
  scudo::MemMapT Regions[NumRegions];
  for (scudo::uptr I = 0; I < NumRegions; ++I)
    Regions[I] = R.dispatch(Base + I * RegionSize, RegionSize);

  VmoSummary Vmo = {};
  MapSummary Maps = {};
  ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_pri", Vmo));
  EXPECT_EQ(Vmo.Count, 1U);
  EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  EXPECT_EQ(Vmo.CommittedBytes, 0U);
  ASSERT_NO_FATAL_FAILURE(countMapsInRange(Base, TotalSize, Maps));
  EXPECT_EQ(Maps.RwMappings, 0U);
  EXPECT_EQ(Maps.NoAccessMappings, 1U);
  EXPECT_EQ(Maps.Vmars, 0U);

  // Activate 10 regions (Regions[5] with MAP_PRECOMMIT so it is committed
  // before any store), then fault in Regions[0] as well.
  for (scudo::uptr I = 0; I < 10; ++I) {
    const scudo::uptr Flags = (I == 5) ? MAP_PRECOMMIT : 0;
    ASSERT_TRUE(Regions[I].remap(Regions[I].getBase() + PageSize, PageSize,
                                 "scudo:ignored", Flags));
  }
  ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_pri", Vmo));
  EXPECT_EQ(Vmo.CommittedBytes, PageSize);

  const scudo::uptr Addr0 = Regions[0].getBase() + PageSize;
  const scudo::uptr Addr5 = Regions[5].getBase() + PageSize;
  memset(reinterpret_cast<void *>(Addr0), 0xaa, PageSize);
  memset(reinterpret_cast<void *>(Addr5), 0xbb, PageSize);

  // Zero-length release must be a no-op (ZX_VMAR_OP_DECOMMIT rejects size 0).
  Regions[5].releaseAndZeroPagesToOS(Addr5, 0);
  EXPECT_EQ(*reinterpret_cast<unsigned char *>(Addr5), 0xbbU);

  ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_pri", Vmo));
  EXPECT_EQ(Vmo.Count, 1U);
  EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  EXPECT_EQ(Vmo.CommittedBytes, 2 * PageSize);
  // ZX_INFO_PROCESS_MAPS reports each protection range inside the single
  // VmMapping: 10 rw- active ranges separated by 11 --- guard/gap ranges.
  ASSERT_NO_FATAL_FAILURE(countMapsInRange(Base, TotalSize, Maps));
  EXPECT_EQ(Maps.RwMappings, 10U);
  EXPECT_EQ(Maps.NoAccessMappings, 11U);
  EXPECT_EQ(Maps.Vmars, 0U);

  // Unmapping Regions[0] while siblings remain alive must decommit its page
  // without disturbing Regions[5].
  Regions[0].unmap();
  ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_pri", Vmo));
  EXPECT_EQ(Vmo.Count, 1U);
  EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  EXPECT_EQ(Vmo.CommittedBytes, PageSize);
  EXPECT_EQ(*reinterpret_cast<unsigned char *>(Addr5), 0xbbU);

  for (scudo::uptr I = 1; I < NumRegions; ++I)
    Regions[I].unmap();

  ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_pri", Vmo));
  EXPECT_EQ(Vmo.Count, 0U);
  EXPECT_EQ(Vmo.ViaHandleCount, 0U);
}

TEST(ScudoMapTest, FuchsiaFallbackNameStandaloneMapAndOom) {
  const scudo::uptr PageSize = scudo::getPageSizeCached();
  VmoSummary Vmo = {};

  // ReservedMemory with Name == nullptr (used by Secondary) defaults to
  // "scudo:secondary" and holds zero open VMO handles.
  {
    scudo::ReservedMemoryT R;
    ASSERT_TRUE(R.create(/*Addr=*/0U, 3 * PageSize, nullptr));
    const scudo::uptr Base = R.getBase();
    std::vector<zx_info_maps_t> Maps;
    ASSERT_NO_FATAL_FAILURE(queryProcessInfo(ZX_INFO_PROCESS_MAPS, Maps));
    zx_koid_t Koid = ZX_KOID_INVALID;
    for (const zx_info_maps_t &Entry : Maps) {
      if (Entry.type == ZX_INFO_MAPS_TYPE_MAPPING && Entry.base == Base) {
        EXPECT_STREQ(Entry.name, "scudo:secondary");
        Koid = Entry.u.mapping.vmo_koid;
        break;
      }
    }
    ASSERT_NE(Koid, ZX_KOID_INVALID);
    ASSERT_NO_FATAL_FAILURE(summarizeVmoByKoid(Koid, Vmo));
    EXPECT_EQ(Vmo.Count, 1U);
    EXPECT_EQ(Vmo.ViaHandleCount, 0U);

    scudo::MemMapT M = R.dispatch(Base, R.getCapacity());
    ASSERT_TRUE(M.remap(Base + PageSize, 2 * PageSize, "scudo:ignored"));
    memset(reinterpret_cast<void *>(Base + PageSize), 0x42, 2 * PageSize);
    // Partial back-trim unmap must decommit the trimmed page immediately.
    M.unmap(Base + 2 * PageSize, PageSize);
    ASSERT_NO_FATAL_FAILURE(summarizeVmoByKoid(Koid, Vmo));
    EXPECT_EQ(Vmo.CommittedBytes, PageSize);
    M.unmap();
    ASSERT_NO_FATAL_FAILURE(summarizeVmoByKoid(Koid, Vmo));
    EXPECT_EQ(Vmo.Count, 0U);
    EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  }

  // Standalone map() holds zero open VMO handles while mapped.
  {
    scudo::MemMapT M;
    ASSERT_TRUE(M.map(/*Addr=*/0U, 2 * PageSize, "scudo:ut_map"));
    ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_map", Vmo));
    EXPECT_EQ(Vmo.Count, 1U);
    EXPECT_EQ(Vmo.ViaHandleCount, 0U);
    M.unmap();
    ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_map", Vmo));
    EXPECT_EQ(Vmo.Count, 0U);
  }

  // Unused ReservedMemory::release() resets state and destroys the VMO.
  {
    scudo::ReservedMemoryT R;
    ASSERT_TRUE(R.create(/*Addr=*/0U, 2 * PageSize, "scudo:ut_rel"));
    R.release();
    EXPECT_FALSE(R.isCreated());
    ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_rel", Vmo));
    EXPECT_EQ(Vmo.Count, 0U);
    EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  }

  // MAP_ALLOWNOMEM failures leave state clean and leak zero VMOs/handles.
  {
    zx_info_vmar_t RootInfo = {};
    ASSERT_EQ(zx_object_get_info(zx_vmar_root_self(), ZX_INFO_VMAR, &RootInfo,
                                 sizeof(RootInfo), nullptr, nullptr),
              ZX_OK);
    scudo::ReservedMemoryT R;
    EXPECT_FALSE(
        R.create(/*Addr=*/0U, RootInfo.len, "scudo:ut_oom", MAP_ALLOWNOMEM));
    EXPECT_FALSE(R.isCreated());
    scudo::MemMapT M;
    EXPECT_FALSE(
        M.map(/*Addr=*/0U, RootInfo.len, "scudo:ut_oom", MAP_ALLOWNOMEM));
    EXPECT_FALSE(M.isAllocated());
    ASSERT_NO_FATAL_FAILURE(summarizeVmosByName("scudo:ut_oom", Vmo));
    EXPECT_EQ(Vmo.Count, 0U);
    EXPECT_EQ(Vmo.ViaHandleCount, 0U);
  }
}
#endif // SCUDO_FUCHSIA
