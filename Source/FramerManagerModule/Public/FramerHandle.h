// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "FramerHandle.generated.h"

DECLARE_DYNAMIC_DELEGATE(FFramerDynamicDelegate);

/** Unique handle that can be used to distinguish framers that have identical delegates. */
USTRUCT(BlueprintType)
struct FFramerHandle
{
	GENERATED_BODY()

	friend class FFramerManager;
	friend struct FFramerHeapOrder;

	FFramerHandle()
		: Handle(0)
	{
	}

	/** True if this handle was ever initialized by the framer manager */
	bool IsValid() const
	{
		return Handle != 0;
	}

	/** Explicitly clear handle */
	void Invalidate()
	{
		Handle = 0;
	}

	bool operator==(const FFramerHandle& Other) const
	{
		return Handle == Other.Handle;
	}

	bool operator!=(const FFramerHandle& Other) const
	{
		return Handle != Other.Handle;
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("%llu"), Handle);
	}

private:
	static constexpr uint32 IndexBits = 24;
	static constexpr uint32 SerialNumberBits = 40;

	static_assert(IndexBits + SerialNumberBits == 64, "The space for the framer index and serial number should total 64 bits");

	static constexpr int32  MaxIndex = (int32)1 << IndexBits;
	static constexpr uint64 MaxSerialNumber = (uint64)1 << SerialNumberBits;

	void SetIndexAndSerialNumber(int32 Index, uint64 SerialNumber)
	{
		check(Index >= 0 && Index < MaxIndex);
		check(SerialNumber < MaxSerialNumber);
		Handle = (SerialNumber << IndexBits) | (uint64)(uint32)Index;
	}

	inline int32 GetIndex() const
	{
		return (int32)(Handle & (uint64)(MaxIndex - 1));
	}

	inline uint64 GetSerialNumber() const
	{
		return Handle >> IndexBits;
	}

	UPROPERTY(Transient)
	uint64 Handle;

	friend uint32 GetTypeHash(const FFramerHandle& InHandle)
	{
		return GetTypeHash(InHandle.Handle);
	}
};

/**
 * A pool based on chunked structure whose element addresses are stable after allocation.
 *
 * Internally stores elements across fixed-size chunks. Chunk pointers are stored
 * in a fixed-size C array (ChunkPointers[MAX_CHUNKS]) that is never reallocated,
 * so once a chunk is allocated its pointer never moves.
 *
 * Thread-safety guarantees:
 *   - operator[] is lock-free for any index within the currently allocated range.
 *   - AddChunk must be externally synchronized (e.g. via FCriticalSection).
 *
 * Not copyable, not movable (address stability constraint).
 */
template<typename T>
class TFramerPool
{
public:
	// Max elements = MAX_CHUNKS * CHUNK_SIZE = 4096 * 4096 = 16M
	static constexpr int32 MAX_CHUNKS = 4096;
	static constexpr int32 CHUNK_SIZE = 4096;

	TFramerPool()
	{
		FMemory::Memzero(ChunkPointers, sizeof(ChunkPointers));
		ChunkCount.store(0, std::memory_order_relaxed);
		NumElements.store(0, std::memory_order_relaxed);
	}

	~TFramerPool()
	{
		int32 Count = ChunkCount.load(std::memory_order_relaxed);
		for (int32 i = 0; i < Count; ++i)
		{
			delete[] ChunkPointers[i];
			ChunkPointers[i] = nullptr;
		}
	}

	// Non-copyable, non-movable
	TFramerPool(const TFramerPool&) = delete;
	TFramerPool& operator=(const TFramerPool&) = delete;
	TFramerPool(TFramerPool&&) = delete;
	TFramerPool& operator=(TFramerPool&&) = delete;

	/**
	 * Access element by index. The chunk pointer is stable once written,
	 * so this is safe to call from any thread for indices within the allocated range.
	 */
	FORCEINLINE T& operator[](int32 Index)
	{
		check(Index >= 0 && Index < NumElements.load(std::memory_order_relaxed));
		return ChunkPointers[Index / CHUNK_SIZE][Index % CHUNK_SIZE];
	}

	FORCEINLINE const T& operator[](int32 Index) const
	{
		check(Index >= 0 && Index < NumElements.load(std::memory_order_relaxed));
		return ChunkPointers[Index / CHUNK_SIZE][Index % CHUNK_SIZE];
	}

	/** Returns the total number of addressable elements. */
	FORCEINLINE int32 Num() const
	{
		return NumElements.load(std::memory_order_relaxed);;
	}

	/** Returns the Index is valid or not. */
	FORCEINLINE bool IsValidIndex(int32 Index) const
	{
		return Index >= 0 && Index < NumElements.load(std::memory_order_relaxed);;
	}

	/**
	 * Append a single chunk and return the index of its first element.
	 * Must be externally synchronized.
	 */
	int32 AddChunk()
	{
		int32 CurrentChunks = ChunkCount.load(std::memory_order_relaxed);
		check(CurrentChunks < MAX_CHUNKS);

		int32 FirstIndex = CurrentChunks * CHUNK_SIZE;

		ChunkPointers[CurrentChunks] = new T[CHUNK_SIZE];
		ChunkCount.store(CurrentChunks + 1, std::memory_order_release);

		NumElements.store((CurrentChunks + 1) * CHUNK_SIZE, std::memory_order_release);
		return FirstIndex;
	}

private:
	/** Fixed-size array of chunk pointers. Never reallocated. */
	T* ChunkPointers[MAX_CHUNKS];

	/** Number of allocated chunks. Written with release, read with acquire. */
	std::atomic<int32> ChunkCount;

	/** Total addressable element count. Written with release, read with acquire. */
	std::atomic<int32> NumElements;
};
