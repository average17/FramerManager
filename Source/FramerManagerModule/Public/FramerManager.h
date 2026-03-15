// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FramerHandle.h"
#include "Misc/RWSpinLock.h"

// using "not checked" user policy (means race detection is disabled) because this delegate is stored in a TSparseArray and causes its reallocation
// from inside delegate's execution. This is incompatible with race detection that needs to access the delegate instance after its execution
using FFramerDelegate = TDelegate<void(), FNotThreadSafeNotCheckedDelegateUserPolicy>;
using FFramerFunction = TFunction<void(void)>;
using FFramerDelegateVariant = TVariant<TYPE_OF_NULLPTR, FFramerDelegate, FFramerDynamicDelegate, FFramerFunction>;

#ifndef UE_ENABLE_TRACKING_FRAMER_SOURCES
	#define UE_ENABLE_TRACKING_FRAMER_SOURCES !UE_BUILD_SHIPPING
#endif

enum class ELevelCollectionType : uint8;
struct FFramerSourceList;

/** Simple interface to wrap a framer delegate that can be either native or dynamic. */
struct FFramerUnifiedDelegate
{
	/** Holds the delegate to call. */
	FFramerDelegateVariant VariantDelegate;

	FFramerUnifiedDelegate() {};

	FFramerUnifiedDelegate(FFramerDelegate const& D) : VariantDelegate(TInPlaceType<FFramerDelegate>(), D) {}
	FFramerUnifiedDelegate(FFramerDynamicDelegate const& D) : VariantDelegate(TInPlaceType<FFramerDynamicDelegate>(), D) {}
	FFramerUnifiedDelegate(FFramerFunction&& Callback) : VariantDelegate(TInPlaceType<FFramerFunction>(), MoveTemp(Callback)) {}
	
	void Execute() const;

	bool IsBound() const;

	const void* GetBoundObject() const;

	void Unbind()
	{
		VariantDelegate.Set<TYPE_OF_NULLPTR>(nullptr);
	}

	/** Utility to output info about delegate as a string. */
	FString ToString() const;

	// Movable only
	FFramerUnifiedDelegate(FFramerUnifiedDelegate&&) = default;
	FFramerUnifiedDelegate(const FFramerUnifiedDelegate&) = delete;
	FFramerUnifiedDelegate& operator=(FFramerUnifiedDelegate&&) = default;
	FFramerUnifiedDelegate& operator=(const FFramerUnifiedDelegate&) = delete;
};

enum class EFramerStatus : uint16
{
	Pending,
	Active,
	Paused,
	Executing,
	ActivePendingRemoval,
	// --- Thread-safety extensions ---
	PendingAdd,             // Slot allocated, awaiting game-thread Add command processing
	PendingRemoval,         // Cross-thread ClearFramer marked, awaiting game-thread removal
	PendingPause,           // Cross-thread PauseFramer marked, awaiting game-thread processing
	PendingUnpause,         // Cross-thread UnPauseFramer marked, awaiting game-thread processing
	Invalid,                // Free slot
};

struct FFramerData
{
	/** Frame interval for executing framer. */
	int32 Interval;

	/** Total frame number that this framer should execute. If -1, it means this framer will repeat until removed. */
	int32 Total;

	/** Remaining frame that this framer should expire. */
	int32 Remaining;

	/** Remaining frame that this framer should execute. */
	int32 RemainingInterval;

	/** If true, this framer was created with a delegate to call (which means if the delegate becomes invalid, we should invalidate the framer too). */
	uint8 bRequiresDelegate : 1;

	/** If true, this framer is important and will be executed before all non-important framers in the same frame. */
	uint8 bIsImportant : 1;

	/** Number of frames waited since last execution, used for priority calculation to prevent starvation. */
	int32 WaitedFrames;

	/** Framer Status. */
	std::atomic<EFramerStatus> Status{EFramerStatus::Invalid};

	/** The level collection that was active when this framer was created. Used to set the correct context before executing the framer's delegate. */
	ELevelCollectionType LevelCollection;

	/** Holds the delegate to call. */
	FFramerUnifiedDelegate FramerDelegate;

	/** Handle representing this framer */
	FFramerHandle Handle;

	/** This is the key to the FramerIndicesByObject map - this is kept so that we can look up even if the referenced object is expired */
	const void* FramerIndicesByObjectKey = nullptr;

	FRAMERMANAGERMODULE_API FFramerData();

	FFramerData(FFramerData&& Other) noexcept
		: Interval(Other.Interval)
		, Total(Other.Total)
		, Remaining(Other.Remaining)
		, RemainingInterval(Other.RemainingInterval)
		, bRequiresDelegate(Other.bRequiresDelegate)
		, bIsImportant(Other.bIsImportant)
		, WaitedFrames(Other.WaitedFrames)
		, LevelCollection(Other.LevelCollection)
		, FramerDelegate(MoveTemp(Other.FramerDelegate))
		, Handle(MoveTemp(Other.Handle))
		, FramerIndicesByObjectKey(Other.FramerIndicesByObjectKey)
	{
		Status.store(Other.Status.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	FFramerData& operator=(FFramerData&& Other) noexcept
	{
		if (this != &Other)
		{
			Interval = Other.Interval;
			Total = Other.Total;
			Remaining = Other.Remaining;
			RemainingInterval = Other.RemainingInterval;
			bRequiresDelegate = Other.bRequiresDelegate;
			bIsImportant = Other.bIsImportant;
			WaitedFrames = Other.WaitedFrames;
			LevelCollection = Other.LevelCollection;
			FramerDelegate = MoveTemp(Other.FramerDelegate);
			Handle = MoveTemp(Other.Handle);
			FramerIndicesByObjectKey = Other.FramerIndicesByObjectKey;
			
			Status.store(Other.Status.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
		return *this;
	}

	// Non-copyable
	FFramerData(const FFramerData&) = delete;
	FFramerData& operator=(const FFramerData&) = delete;
};

/**
 * Command queued from any thread and consumed by the game thread in ProcessPendingCommands().
 */
struct FFramerCommand
{
	enum class EType : uint8
	{
		Set,
		Clear,
		Pause,
		Unpause,
		ClearByObject,
	};

	EType Type;
	FFramerHandle Handle;        // Used by Set/Clear/Pause/Unpause
	const void* Object = nullptr; // Used by ClearByObject
};

/**
 * Class to globally manage framers.
 * Thread-safe: any thread may call SetFramer/ClearFramer/Pause/UnPause/queries.
 * Callbacks always execute on the game thread during Tick().
 */
class FFramerManager : public FTickableGameObject, public FNoncopyable
{
public:
	static FRAMERMANAGERMODULE_API FFramerManager* Get(UWorld* InWorld);
	static FRAMERMANAGERMODULE_API FFramerManager* Get(const UObject* WorldContextObject);
	static void OnStartup();
	static void OnShutdown();

	// FTickableGameObject interface
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual UWorld* GetTickableGameObjectWorld() const override { return OwnerWorld; };
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FFramerManager, STATGROUP_Tickables); }
	virtual bool IsTickableWhenPaused() const override;
	virtual void Tick(float DeltaSeconds) override;

private:
	// Callback function registered with global world delegates to instantiate world manager when a game world is created
	static void OnWorldInit(UWorld* World, const UWorld::InitializationValues IVS);

	// Callback function registered with global world delegates to cleanup world manager contents
	static void OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);

public:
	// ----------------------------------
	// Framer API
	
	FRAMERMANAGERMODULE_API FFramerManager();
	FRAMERMANAGERMODULE_API virtual ~FFramerManager();

	/**
	 * Called from crash handler to provide more debug information.
	 */
	FRAMERMANAGERMODULE_API void OnCrash();

	/**
	 * Sets a framer to repeatedly call the given native function at a set interval untill Total.  
	 * If a framer is already set for this handle, it will replace the current framer.
	 *
	 * @param InOutHandle			If the passed-in handle refers to an existing framer, it will be cleared before the new framer is added. A new handle to the new framer is returned in either case.
	 * @param InObj					Object to call the framer function on.
	 * @param InFramerMethod		Method to call when framer fires.
	 * @param InInterval			The amount of frame interval for execution.  If <= 0, clears existing framers.
	 * @param InTotal				Total frame number of this framer to execute. If -1, it means this framer will repeat until removed.
	 * @param InFirstDelay			The frame for the first iteration of a looping framer. If <= 0 InInterval will be used.
	 * @param bInIsImportant		If true, this framer will be executed before non-important framers and will not be skipped due to budget limits.
	 */
	template< class UserClass >
	inline void SetFramer(FFramerHandle& InOutHandle, UserClass* InObj, typename FFramerDelegate::TMethodPtr<UserClass> InFramerMethod, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(FFramerDelegate::CreateUObject(InObj, InFramerMethod)), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}
	template< class UserClass >
	inline void SetFramer(FFramerHandle& InOutHandle, UserClass* InObj, typename FFramerDelegate::TConstMethodPtr<UserClass> InFramerMethod, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(FFramerDelegate::CreateUObject(InObj, InFramerMethod)), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}

	/** Version that takes any generic delegate. */
	inline void SetFramer(FFramerHandle& InOutHandle, FFramerDelegate const& InDelegate, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(InDelegate), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}
	/** Version that takes a dynamic delegate (e.g. for UFunctions). */
	inline void SetFramer(FFramerHandle& InOutHandle, FFramerDynamicDelegate const& InDynDelegate, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(InDynDelegate), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}
	/*** Version that doesn't take a delegate */
	inline void SetFramer(FFramerHandle& InOutHandle, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}
	/** Version that takes a TFunction */
	inline void SetFramer(FFramerHandle& InOutHandle, TFunction<void(void)>&& Callback, int32 InInterval, int32 InTotal, int32 InFirstDelay = 0, bool bInIsImportant = false)
	{
		InternalSetFramer(InOutHandle, FFramerUnifiedDelegate(MoveTemp(Callback)), InInterval, InTotal, InFirstDelay, bInIsImportant);
	}

	/**
	* Clears a previously set framer, identical to calling SetFramer() with a <= 0.f rate.
	* Invalidates the framer handle as it should no longer be used.
	*
	* @param InHandle The handle of the framer to clear.
	*/
	FRAMERMANAGERMODULE_API void ClearFramer(FFramerHandle& InHandle);

	/** Clears all framers that are bound to functions on the given object.*/
	FRAMERMANAGERMODULE_API void ClearAllFramersForObject(void const* Object);
	
	/**
	 * Returns true if the specified framer exists.
	 *
	 * @param InHandle The handle of the framer to check for existence.
	 * @return true if the framer exists, false otherwise.
	 */
	FRAMERMANAGERMODULE_API bool FramerExists(FFramerHandle InHandle) const;

	/**
	 * Pauses a previously set framer.
	 *
	 * @param InHandle The handle of the framer to pause.
	 */
	FRAMERMANAGERMODULE_API void PauseFramer(FFramerHandle InHandle);

	/**
	 * Unpauses a previously set framer
	 *
	 * @param InHandle The handle of the framer to unpause.
	 */
	FRAMERMANAGERMODULE_API void UnPauseFramer(FFramerHandle InHandle);

	/**
	 * Gets the current interval (frame between activations) for the specified framer.
	 *
	 * @param InHandle The handle of the framer to return the rate of.
	 * @return The current rate or -1 if framer does not exist.
	 */
	FRAMERMANAGERMODULE_API int32 GetFramerInterval(FFramerHandle InHandle) const;

	/**
	 * Returns true if the specified framer exists and is not paused.
	 *
	 * @param InHandle The handle of the framer to check for being active.
	 * @return true if the framer exists and is active, false otherwise.
	 */
	FRAMERMANAGERMODULE_API bool IsFramerActive(FFramerHandle InHandle) const;

	/**
	 * Returns true if the specified framer exists and is paused.
	 *
	 * @param InHandle The handle of the framer to check for being paused.
	 * @return true if the framer exists and is paused, false otherwise.
	 */
	FRAMERMANAGERMODULE_API bool IsFramerPaused(FFramerHandle InHandle) const;

	/**
	 * Returns true if the specified framer exists and is pending.
	 *
	 * @param InHandle The handle of the framer to check for being pending.
	 * @return true if the framer exists and is pending, false otherwise.
	 */
	FRAMERMANAGERMODULE_API bool IsFramerPending(FFramerHandle InHandle) const;

	/**
	 * Gets the current elapsed frame for the specified framer.
	 *
	 * @param InHandle The handle of the framer to check the elapsed frame of.
	 * @return The current frame elapsed or -1 if the framer does not exist.
	 */
	FRAMERMANAGERMODULE_API int32 GetFramerElapsed(FFramerHandle InHandle) const;

	/**
	 * Gets the frame remaining before the specified framer is called
	 *
	 * @param InHandle The handle of the framer to check the remaining frame of.
	 * @return	 The current frame remaining, or -1 if framer does not exist
	 */
	FRAMERMANAGERMODULE_API int32 GetFramerRemaining(FFramerHandle InHandle) const;

	/**
	 * Gets the total frame of the timer should execute
	 *
	 * @param InHandle The handle of the framer to check the total time of.
	 * @return	 The total frame, or -1 if framer will repeatedly execute until removed
	 */
	FRAMERMANAGERMODULE_API int32 GetTimerTotal(FFramerHandle InHandle) const;

	inline bool HasBeenTickedThisFrame() const
	{
		return (LastTickedFrame == GFrameCounter);
	}

	/**
	 * Finds a handle to a framer bound to a particular dynamic delegate.
	 * This function is intended to be used only by the K2 system.
	 * Game-thread only.
	 *
	 * @param  InDynamicDelegate  The dynamic delegate to search for.
	 *
	 * @return A handle to the found framer - !IsValid() if no such framer was found.
	 */
	FRAMERMANAGERMODULE_API FFramerHandle K2_FindDynamicFramerHandle(FFramerDynamicDelegate InDynamicDelegate) const;

	/** Debug command to output info on all framers currently set to the log. */
	FRAMERMANAGERMODULE_API void ListFramers() const;

private:
	void ExecuteFramerHeap(TArray<FFramerHandle>& FramerHeap, int32& OutExecutedCount, double& InOutRemainingBudgetMs, bool bTrackBudget, TArray<TPair<FString, double>>& InOutExpensiveFramerInfos);
	FRAMERMANAGERMODULE_API void InternalSetFramer(FFramerHandle& InOutHandle, FFramerUnifiedDelegate&& InDelegate, int32 InInterval, int32 InTotal, int32 InFirstDelay, bool bInIsImportant);
	FRAMERMANAGERMODULE_API void InternalClearFramer(FFramerHandle InHandle);
	FRAMERMANAGERMODULE_API void InternalClearAllFramers(void const* Object);

	/** Removes a framer from the Framer list at the given index, also cleaning up the FramerIndicesByObject map */
	FRAMERMANAGERMODULE_API void RemoveFramer(FFramerHandle const& Handle);
	/** Internal game-thread only cleanup - does not enqueue command */
	FRAMERMANAGERMODULE_API void RemoveFramerInternal(FFramerHandle const& Handle);
	FRAMERMANAGERMODULE_API bool WillRemoveFramerAssert(FFramerHandle const& Handle) const;

	/** Will find a framer in the active, paused, or pending list. */
	inline FFramerData const* FindFramer(FFramerHandle const& InHandle) const
	{
		return const_cast<FFramerManager*>(this)->FindFramer(InHandle);
	}
	FRAMERMANAGERMODULE_API FFramerData* FindFramer(FFramerHandle const& InHandle);

	/** Thread-safe framer lookup — validates index range + serial number + atomic status */
	FRAMERMANAGERMODULE_API FFramerData* FindFramerThreadSafe(FFramerHandle const& InHandle);
	
	/** Generates a handle for a framer at a given index */
	FFramerHandle GenerateHandle(int32 Index);

	bool FramerCanExecute(FFramerData const* const FramerData) const;
	
	/** Allocate a framer slot from the free list, expanding Framers if needed. Thread-safe. */
	FRAMERMANAGERMODULE_API int32 AllocateFramerSlot();

	/** Return a framer slot to the free list. Thread-safe. */
	FRAMERMANAGERMODULE_API void RecycleFramerSlot(int32 Index);

	/** Consumes the MPSC command queue on the game thread. */
	FRAMERMANAGERMODULE_API void ProcessPendingCommands();
	
	static TMap<UWorld*, FFramerManager*> FramerManagers;
	static FDelegateHandle OnWorldInitHandle;
	static FDelegateHandle OnWorldCleanupHandle;

	// >>> Thread-safe data members
	/** MPSC command queue: producers = any thread, consumer = game thread Tick(). */
	TQueue<FFramerCommand, EQueueMode::Mpsc> CommandQueue;

	/** Lock-free free-list of available slot indices. */
	TLockFreePointerListLIFO<uint32> FreeFramers;
	
	/** The array of framers - all other arrays will index into this. Uses chunked storage for address stability. */
	TFramerPool<FFramerData> FramerSlots;

	/** Current high-water mark of allocated slot indices. Used for lock-free range checks. */
	std::atomic<int32> FramerSlotsNum{0};

	/** Mutex protecting Framers expansion (rare). No read lock needed — chunk addresses are stable. */
	FCriticalSection FramersExpandLock;

	/** Lightweight spin read-write lock protecting Remaining/RemainingInterval field reads/writes (frequent, cheap). */
	mutable UE::FRWSpinLock FramerFieldLock;
	// <<< Thread-safe data members

	/** Heap of actively running framers. */
	TArray<FFramerHandle> ActiveFramerHeap;
	
	/** Heap of important framers that must be executed every frame regardless of budget. */
	TArray<FFramerHandle> ImportantFramerHeap;
	
	/** Set of paused framers. */
	TSet<FFramerHandle> PausedFramerSet;
	/** Set of framers added this frame, to be added after framer has been ticked */
	TSet<FFramerHandle> PendingFramerSet;
	/** A map of object pointers to framers with delegates bound to those objects, for quick lookup */
	TMap<const void*, TSet<FFramerHandle>> ObjectToFramers;

	/** Index to the framer delegate currently being executed, or INDEX_NONE if none are executing.  Used to handle "framer delegates that manipulate framers" cases. */
	FFramerHandle CurrentlyExecutingFramer;

	/** Set this to GFrameCounter when Framer is ticked. To figure out if Framer has been already ticked or not this frame. */
	uint64 LastTickedFrame = 0;
	
	/** The last serial number we assigned from framer manager */
	static FRAMERMANAGERMODULE_API std::atomic_uint64_t LastAssignedSerialNumber;

	/** The world that created this framer manager. May be null if this framer manager wasn't created by a world. */
	UWorld* OwnerWorld = nullptr;

#if UE_ENABLE_TRACKING_FRAMER_SOURCES
	/** Debugging/tracking information used when FramerManager.BuildFramerSourceList is set */
	TUniquePtr<FFramerSourceList> FramerSourceList;
#endif
};
