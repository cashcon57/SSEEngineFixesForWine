#pragma once

#include <mimalloc.h>

// Wine-compatible memory allocator replacement for SSE Engine Fixes.
//
// The original SSE Engine Fixes replaces Skyrim's MemoryManager with Intel TBB
// (scalable_malloc/scalable_free). TBB crashes under Wine/CrossOver/Proton due
// to incompatible thread-local storage and memory pool initialization.
//
// This implementation replaces the allocator with mimalloc, which uses
// per-thread caches and avoids the global serialized lock that HeapAlloc
// incurs. mimalloc initializes lazily via VirtualAlloc/FlsAlloc — no DLL
// load-phase TLS issues. Expected improvement: 10-30% faster form loading
// for large modlists (1000+ plugins) vs the previous HeapAlloc approach.
//
// Additionally, the per-thread ScrapHeap reserve is expanded from 64MB to
// 512MB (configurable) to accommodate heavy loading with many plugins.

namespace Patches::WineMemoryManager
{
	namespace detail
	{
		// Header prepended to every allocation from mimalloc.
		// 32 bytes to maintain 16-byte alignment of the user pointer.
		// The header sits immediately before the user pointer so we can
		// identify our allocations in Deallocate and recover the raw
		// mi_malloc pointer for mi_free.
		struct AllocHeader {
			static constexpr std::uint64_t MAGIC = 0x574E4D454D414C43ULL;  // "WNMEMALC"

			std::uint64_t magic;       // 0x00: identifies as our allocation
			std::uint64_t userSize;    // 0x08: requested allocation size (for Reallocate memcpy)
			void*         rawPtr;      // 0x10: original mi_malloc pointer (for mi_free)
			std::uint64_t pad;         // 0x18: alignment padding to 32 bytes
		};
		static_assert(sizeof(AllocHeader) == 32);

		// Statistics (atomic for thread-safety, relaxed ordering for perf)
		inline std::atomic<std::uint64_t> g_totalAllocated{ 0 };
		inline std::atomic<std::uint64_t> g_totalFreed{ 0 };
		inline std::atomic<std::uint64_t> g_allocationCount{ 0 };
		inline std::atomic<std::uint64_t> g_freeCount{ 0 };
		inline std::atomic<std::uint64_t> g_originalFreeCount{ 0 };
		inline std::atomic<std::uint64_t> g_failCount{ 0 };
		inline std::atomic<std::uint64_t> g_scrapHeapExpansions{ 0 };

		// SafetyHook inline hooks
		inline SafetyHookInline g_allocateHook{};
		inline SafetyHookInline g_deallocateHook{};
		inline SafetyHookInline g_reallocateHook{};
		inline SafetyHookInline g_getScrapHeapHook{};

		inline AllocHeader* GetHeader(void* a_userPtr)
		{
			return reinterpret_cast<AllocHeader*>(
				static_cast<char*>(a_userPtr) - sizeof(AllocHeader));
		}

		inline bool IsOurAllocation(void* a_userPtr)
		{
			if (!a_userPtr) return false;
			auto* header = GetHeader(a_userPtr);
			return header->magic == AllocHeader::MAGIC;
		}

		// MemoryManager::Allocate replacement
		// x64: this=RCX, a_size=RDX, a_alignment=R8D, a_aligned=R9B
		inline void* HookedAllocate(
			RE::MemoryManager* a_self,
			std::size_t        a_size,
			std::int32_t       a_alignment,
			bool               a_aligned)
		{
			if (a_size == 0) a_size = 1;

			// Effective alignment: minimum 16 (x64 standard), higher if requested
			std::size_t align = 16;
			if (a_aligned && a_alignment > 16) {
				align = static_cast<std::size_t>(a_alignment);
			}

			// Total: header (32) + alignment padding + user data
			std::size_t totalSize = sizeof(AllocHeader) + align + a_size;
			// v1.22.93: Use mi_zalloc (zeroed allocation) instead of mi_malloc.
			// BSTHashMap::grow allocates a new entry array and updates the
			// _entries pointer BEFORE initializing entries. Concurrent find()
			// threads can read the new array while it has uninitialized "next"
			// pointers — garbage values that happen to be valid addresses form
			// infinite cycles. Zeroing ensures uninitialized next = nullptr,
			// which faults to VEH → g_zeroPage → sentinel → loop exits.
			void* raw = mi_zalloc(totalSize);
			if (!raw) {
				g_failCount.fetch_add(1, std::memory_order_relaxed);
				return nullptr;
			}

			// Align user pointer: skip past header, then round up to alignment
			std::uintptr_t rawAddr = reinterpret_cast<std::uintptr_t>(raw);
			std::uintptr_t userAddr = (rawAddr + sizeof(AllocHeader) + align - 1) & ~(align - 1);

			// Write header in the 32 bytes immediately before user pointer
			auto* header = reinterpret_cast<AllocHeader*>(userAddr - sizeof(AllocHeader));
			header->magic = AllocHeader::MAGIC;
			header->userSize = a_size;
			header->rawPtr = raw;
			header->pad = 0;

			g_totalAllocated.fetch_add(a_size, std::memory_order_relaxed);
			g_allocationCount.fetch_add(1, std::memory_order_relaxed);

			return reinterpret_cast<void*>(userAddr);
		}

		// MemoryManager::Deallocate replacement
		inline void HookedDeallocate(
			RE::MemoryManager* a_self,
			void*              a_mem,
			bool               a_aligned)
		{
			if (!a_mem) return;

			if (IsOurAllocation(a_mem)) {
				auto* header = GetHeader(a_mem);
				std::size_t size = header->userSize;
				void* raw = header->rawPtr;

				// Clear magic to prevent double-free
				header->magic = 0;

				mi_free(raw);

				g_totalFreed.fetch_add(size, std::memory_order_relaxed);
				g_freeCount.fetch_add(1, std::memory_order_relaxed);
			} else {
				// Pre-existing allocation from before our hooks — use original
				g_originalFreeCount.fetch_add(1, std::memory_order_relaxed);
				g_deallocateHook.call<void>(a_self, a_mem, a_aligned);
			}
		}

		// MemoryManager::Reallocate replacement
		inline void* HookedReallocate(
			RE::MemoryManager* a_self,
			void*              a_oldMem,
			std::size_t        a_newSize,
			std::int32_t       a_alignment,
			bool               a_aligned)
		{
			if (!a_oldMem) {
				return HookedAllocate(a_self, a_newSize, a_alignment, a_aligned);
			}
			if (a_newSize == 0) {
				HookedDeallocate(a_self, a_oldMem, a_aligned);
				return nullptr;
			}

			if (IsOurAllocation(a_oldMem)) {
				auto* oldHeader = GetHeader(a_oldMem);
				std::size_t oldSize = oldHeader->userSize;

				void* newMem = HookedAllocate(a_self, a_newSize, a_alignment, a_aligned);
				if (newMem) {
					std::memcpy(newMem, a_oldMem, (std::min)(oldSize, a_newSize));
					HookedDeallocate(a_self, a_oldMem, a_aligned);
				}
				return newMem;
			}

			// Pre-existing allocation — use original reallocator
			return g_reallocateHook.call<void*>(a_self, a_oldMem, a_newSize, a_alignment, a_aligned);
		}

		// MemoryManager::GetThreadScrapHeap hook — expand reserve before first use
		inline RE::ScrapHeap* HookedGetThreadScrapHeap(RE::MemoryManager* a_self)
		{
			auto* heap = g_getScrapHeapHook.call<RE::ScrapHeap*>(a_self);
			if (heap && heap->baseAddress == nullptr) {
				// ScrapHeap not yet initialized — expand reserve before first VirtualAlloc
				std::size_t targetBytes = static_cast<std::size_t>(
					Settings::Memory::uScrapHeapSizeMB.GetValue()) << 20;
				if (heap->reserveSize < targetBytes) {
					heap->reserveSize = targetBytes;
					g_scrapHeapExpansions.fetch_add(1, std::memory_order_relaxed);
				}
			}
			return heap;
		}
	}

	inline void LogStats()
	{
		auto allocated = detail::g_totalAllocated.load(std::memory_order_relaxed);
		auto freed = detail::g_totalFreed.load(std::memory_order_relaxed);
		auto allocCount = detail::g_allocationCount.load(std::memory_order_relaxed);
		auto freeCount = detail::g_freeCount.load(std::memory_order_relaxed);
		auto origFreeCount = detail::g_originalFreeCount.load(std::memory_order_relaxed);
		auto failCount = detail::g_failCount.load(std::memory_order_relaxed);
		auto scrapExpansions = detail::g_scrapHeapExpansions.load(std::memory_order_relaxed);

		double allocMB = static_cast<double>(allocated) / (1024.0 * 1024.0);
		double freedMB = static_cast<double>(freed) / (1024.0 * 1024.0);
		double liveMB = static_cast<double>(allocated - freed) / (1024.0 * 1024.0);

		logger::info("WineMemoryManager stats: {:.1f}MB allocated, {:.1f}MB freed, {:.1f}MB live",
			allocMB, freedMB, liveMB);
		logger::info("  {} allocations, {} frees (ours), {} frees (original), {} failures",
			allocCount, freeCount, origFreeCount, failCount);
		if (scrapExpansions > 0) {
			logger::info("  {} ScrapHeap expansions to {}MB",
				scrapExpansions, Settings::Memory::uScrapHeapSizeMB.GetValue());
		}
	}

	inline void Install()
	{
		bool replaceAllocator = Settings::Memory::bReplaceAllocator.GetValue();
		bool expandScrapHeap = Settings::Memory::bExpandScrapHeap.GetValue();

		if (replaceAllocator) {
			// Hook MemoryManager::Allocate
			const REL::Relocation allocate{ RELOCATION_ID(66859, 68115) };
			detail::g_allocateHook = safetyhook::create_inline(
				allocate.address(), detail::HookedAllocate);

			// Hook MemoryManager::Deallocate
			const REL::Relocation deallocate{ RELOCATION_ID(66861, 68117) };
			detail::g_deallocateHook = safetyhook::create_inline(
				deallocate.address(), detail::HookedDeallocate);

			// Hook MemoryManager::Reallocate
			const REL::Relocation reallocate{ RELOCATION_ID(66860, 68116) };
			detail::g_reallocateHook = safetyhook::create_inline(
				reallocate.address(), detail::HookedReallocate);

			logger::info("installed Wine-compatible memory allocator (mimalloc)"sv);
		}

		if (expandScrapHeap) {
			// Hook GetThreadScrapHeap to expand reserve before first allocation.
			// Only affects ScrapHeaps created AFTER our hook — the main thread's
			// ScrapHeap may already be initialized from early game init.
			const REL::Relocation getScrapHeap{ RELOCATION_ID(66841, 68088) };
			detail::g_getScrapHeapHook = safetyhook::create_inline(
				getScrapHeap.address(), detail::HookedGetThreadScrapHeap);

			logger::info("installed ScrapHeap expansion ({}MB)"sv,
				Settings::Memory::uScrapHeapSizeMB.GetValue());
		}
	}
}
