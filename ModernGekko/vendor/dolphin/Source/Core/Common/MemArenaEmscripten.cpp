// Copyright 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemArena.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

// wasm32 has one linear memory, no virtual address space to reserve, and no way
// to make one page of memory visible at two addresses. Every other MemArena
// implementation is built on exactly those three things.
//
// What survives is the part that matters here. The *physical* regions -- RAM,
// L1 cache, EXRAM -- are each mapped once, so they only need to be memory;
// handing out offsets into one flat allocation satisfies that completely.
//
// What does not survive is fastmem: the 4 GB reservation with the emulated
// address space mapped into it, so that generated code can dereference a guest
// pointer without a translation call. That is a virtual-memory trick and there
// is no wasm equivalent. It is also not needed here, because fastmem exists to
// serve a JIT, and a wasm build has no JIT -- guest code arrives as a
// recompiled module (or the interpreter), and both go through MMU::Read_U32
// and friends, which handle the mirrors in software. So the reservation calls
// report failure, MemoryManager leaves m_is_fastmem_arena_initialized false,
// and every user of it is already guarded on that flag.

namespace Common
{
MemArena::MemArena() = default;
MemArena::~MemArena() = default;

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
  ReleaseSHMSegment();
  m_reserved_region = std::calloc(1, size);
  m_reserved_region_size = m_reserved_region ? size : 0;
}

void MemArena::ReleaseSHMSegment()
{
  std::free(m_reserved_region);
  m_reserved_region = nullptr;
  m_reserved_region_size = 0;
}

void* MemArena::CreateView(s64 offset, size_t size)
{
  // A view is a window onto the segment, not a separate allocation: the caller
  // maps each physical region exactly once, so a pointer into the segment is a
  // complete answer.
  if (!m_reserved_region || offset < 0 ||
      static_cast<size_t>(offset) + size > m_reserved_region_size)
  {
    return nullptr;
  }
  return static_cast<u8*>(m_reserved_region) + offset;
}

void MemArena::ReleaseView(void* view, size_t size)
{
  // The segment owns the memory; a view never did.
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
  // No fastmem. See the comment at the top of this file.
  return nullptr;
}

void MemArena::ReleaseMemoryRegion()
{
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
  return nullptr;
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  return false;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
}

size_t MemArena::GetPageSize() const
{
  return 64 * 1024;  // a wasm page
}

// Lazily-committed memory is a virtual-memory optimisation too. Allocating it
// up front and zeroing it is the same thing observably, only eager.
LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);
  if (size == 0)
    return nullptr;
  void* memory = std::calloc(1, size);
  if (!memory)
    return nullptr;
  m_memory = memory;
  m_size = size;
  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);
  std::memset(m_memory, 0, m_size);
}

void LazyMemoryRegion::Release()
{
  if (!m_memory)
    return;
  std::free(m_memory);
  m_memory = nullptr;
  m_size = 0;
}
}  // namespace Common
