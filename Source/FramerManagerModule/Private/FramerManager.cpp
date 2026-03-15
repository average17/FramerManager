// Copyright Epic Games, Inc. All Rights Reserved.

#include "FramerManager.h"
#include "Misc/TimeGuard.h"
#include "Misc/StringOutputDevice.h"

DEFINE_LOG_CATEGORY_STATIC(LogFramerManager, Log, All);

DECLARE_DELEGATE_RetVal(const FString, FFramerNameDelegate);

DECLARE_CYCLE_STAT(TEXT("SetFramer"), STAT_SetFramer, STATGROUP_Engine);
DECLARE_CYCLE_STAT(TEXT("ClearFramer"), STAT_ClearFramer, STATGROUP_Engine);
DECLARE_CYCLE_STAT(TEXT("ClearAllFramers"), STAT_ClearAllFramers, STATGROUP_Engine);

DECLARE_DWORD_COUNTER_STAT(TEXT("FramerManager Heap Size"),STAT_NumFramerHeapEntries, STATGROUP_Engine);

bool GTickWhenWorldPaused = false;
static FAutoConsoleVariableRef CVarFMTickWhenWorldPaused(
	TEXT("FM.Tick.EnableWhenWorldPaused"),
	GTickWhenWorldPaused,
	TEXT("If true, framer manager will tick even when the world has been paused. \n"),
	ECVF_Default
);

static float DumpFramerLogsThreshold = 0.f;
static FAutoConsoleVariableRef CVarDumpFramerLogsThreshold(
	TEXT("FM.DumpFramerLogsThreshold"), DumpFramerLogsThreshold,
	TEXT("Threshold (in milliseconds) after which we log framer info to try and help track down spikes in the framer code. Disabled when set to 0"),
	ECVF_Default);

static int32 DumpFramerLogResolveVirtualFunctions = 1;
static FAutoConsoleVariableRef CVarFMDumpFramerLogResolveVirtualFunctions(
	TEXT("FM.DumpFramerLogResolveVirtualFunctions"), DumpFramerLogResolveVirtualFunctions,
	TEXT("When logging framer info virtual functions will be resolved, if possible."),
	ECVF_Default);

static int32 DumpFramerLogSymbolNames = 1;
static FAutoConsoleVariableRef CVarFMDumpFramerLogSymbolNames(
	TEXT("FM.DumpFramerLogSymbolNames"), DumpFramerLogSymbolNames,
	TEXT("When logging framer info, symbol names will be included if set to 1."),
	ECVF_Default);

static int32 MaxExpiredFramersToLog = 30;
static FAutoConsoleVariableRef CVarMaxExpiredFramersToLog(
	TEXT("FM.MaxExpiredFramersToLog"), 
	MaxExpiredFramersToLog,
	TEXT("Maximum number of FramerData exceeding the threshold to log in a single frame."));

static float GTickBudgetMs = -1.0f;
static FAutoConsoleVariableRef CVarTickBudgetMs(
	TEXT("FM.Tick.BudgetMs"),
	GTickBudgetMs,
	TEXT("Tick budget in milliseconds. -1 means no budget limit, > 0 means budget limit."),
	ECVF_Default
);

static int32 GStarvationFactor = 1;
static FAutoConsoleVariableRef CVarStarvationFactor(
	TEXT("FM.Tick.StarvationFactor"),
	GStarvationFactor,
	TEXT("Starvation factor for priority calculation."),
	ECVF_Default
);

static bool GBudgetDebugEnabled = false;
static FAutoConsoleVariableRef CVarBudgetDebugEnabled(
	TEXT("FM.Tick.BudgetDebugEnabled"),
	GBudgetDebugEnabled,
	TEXT("Enable budget debug logging or not."),
	ECVF_Default
);

/** Track the last assigned handle globally */
std::atomic_uint64_t FFramerManager::LastAssignedSerialNumber = 0;

void FFramerUnifiedDelegate::Execute() const
{
	switch (VariantDelegate.GetIndex())
	{
	case FFramerDelegateVariant::IndexOfType<FFramerDelegate>():
		{
			const FFramerDelegate& FuncDelegate = VariantDelegate.Get<FFramerDelegate>();

#if UE_WITH_REMOTE_OBJECT_HANDLE
			const FName WorkName{ TEXT("FramerManager::Execute FFramerDelegate") };
			UE::RemoteExecutor::ExecuteTransactional(WorkName, [&FuncDelegate]()
			{
#endif
				if (FuncDelegate.IsBound())
				{
					FScopeCycleCounterUObject Context(FuncDelegate.GetUObject());
					FuncDelegate.Execute();
				}
#if UE_WITH_REMOTE_OBJECT_HANDLE
			});
#endif
			break;
		}
	case FFramerDelegateVariant::IndexOfType<FFramerDynamicDelegate>():
		{
			const FFramerDynamicDelegate& FuncDynDelegate = VariantDelegate.Get<FFramerDynamicDelegate>();
#if UE_WITH_REMOTE_OBJECT_HANDLE
			const FName WorkName { TEXT("FramerManager::Execute FFramerDynamicDelegate") };
			UE::RemoteExecutor::ExecuteTransactional(WorkName, [&FuncDynDelegate]()
			{
#endif
				if (FuncDynDelegate.IsBound())
				{
					// stat scope is handled by UObject::ProcessEvent for the UFunction.
					FuncDynDelegate.ProcessDelegate<UObject>(nullptr);
				}
#if UE_WITH_REMOTE_OBJECT_HANDLE
			});
#endif
			break;
		}
	case FFramerDelegateVariant::IndexOfType<FFramerFunction>():
		{
			if (const FFramerFunction& FramerFunction = VariantDelegate.Get<FFramerFunction>())
			{
				QUICK_SCOPE_CYCLE_COUNTER(STAT_FFramerUnifiedDelegate_Execute);
				FramerFunction();
			}
			break;
		}
	default:
		break;
	}
}

bool FFramerUnifiedDelegate::IsBound() const
{
	switch (VariantDelegate.GetIndex())
	{
	case FFramerDelegateVariant::IndexOfType<FFramerDelegate>():
		return VariantDelegate.Get<FFramerDelegate>().IsBound();
	case FFramerDelegateVariant::IndexOfType<FFramerDynamicDelegate>():
		return VariantDelegate.Get<FFramerDynamicDelegate>().IsBound();
	case FFramerDelegateVariant::IndexOfType<FFramerFunction>():
		return VariantDelegate.Get<FFramerFunction>()!=nullptr;
	default:
		return false;
	}
}

const void* FFramerUnifiedDelegate::GetBoundObject() const
{
	switch (VariantDelegate.GetIndex())
	{
	case FFramerDelegateVariant::IndexOfType<FFramerDelegate>():
		{
			const FFramerDelegate& FuncDelegate = VariantDelegate.Get<FFramerDelegate>();
			if (FuncDelegate.IsBound())
			{
				return FuncDelegate.GetObjectForTimerManager();
			}
			break;
		}
	case FFramerDelegateVariant::IndexOfType<FFramerDynamicDelegate>():
		{
			const FFramerDynamicDelegate& FuncDynDelegate = VariantDelegate.Get<FFramerDynamicDelegate>();
			if (FuncDynDelegate.IsBound())
			{
				return FuncDynDelegate.GetUObject();
			}
			break;
		}
	default:
		break;
	}
	return nullptr;
}

FString FFramerUnifiedDelegate::ToString() const
{
	switch (VariantDelegate.GetIndex())
	{
	case FFramerDelegateVariant::IndexOfType<FFramerDelegate>():
		{
			const FFramerDelegate& FuncDelegate = VariantDelegate.Get<FFramerDelegate>();
			if (FuncDelegate.IsBound())
			{
				FString FunctionNameStr;
				FName FunctionName;
#if USE_DELEGATE_TRYGETBOUNDFUNCTIONNAME
				FunctionName = FuncDelegate.TryGetBoundFunctionName();
#endif
				if (FunctionName.IsNone())
				{
					void** VtableAddr = nullptr;
#if PLATFORM_COMPILER_CLANG || defined(_MSC_VER)
					// Add the vtable address
					const void* UserObject = FuncDelegate.GetObjectForTimerManager();
					if (UserObject)
					{
						VtableAddr = *(void***)UserObject;
						FunctionNameStr = FString::Printf(TEXT("vtbl: %p"), VtableAddr);
					}
#endif // PLATFORM_COMPILER_CLANG

					uint64 ProgramCounter = FuncDelegate.GetBoundProgramCounterForTimerManager();
					if (ProgramCounter != 0)
					{
						// Add the function address

#if PLATFORM_COMPILER_CLANG
						// See if this is a virtual function. Heuristic is that real function addresses are higher than some value, and vtable offsets are lower
						const uint64 MaxVTableAddressOffset = 32768;
						if (DumpFramerLogResolveVirtualFunctions && VtableAddr && ProgramCounter > 0 && ProgramCounter < MaxVTableAddressOffset)
						{
							// If the ProgramCounter is just an offset to the vtable (virtual member function) then resolve the actual ProgramCounter here.
							ProgramCounter = (uint64)VtableAddr[ProgramCounter / sizeof(void*)];
						}
#endif // PLATFORM_COMPILER_CLANG

						FunctionNameStr += FString::Printf(TEXT(" func: 0x%llx"), ProgramCounter);

						if (DumpFramerLogSymbolNames)
						{
							// Try to resolve the function address to a symbol
							FProgramCounterSymbolInfo SymbolInfo;
							SymbolInfo.FunctionName[0] = 0;
							SymbolInfo.Filename[0] = 0;
							SymbolInfo.LineNumber = 0;
							FPlatformStackWalk::ProgramCounterToSymbolInfo(ProgramCounter, SymbolInfo);
							FunctionNameStr += FString::Printf(TEXT(" %s [%s:%d]"), ANSI_TO_TCHAR(SymbolInfo.FunctionName), ANSI_TO_TCHAR(SymbolInfo.Filename), SymbolInfo.LineNumber);
						}
					}
					else
					{
						FunctionNameStr = TEXT(" 0x0");
					}
				}

				const UObject* const Object = FuncDelegate.GetUObject();
				return FString::Printf(TEXT("DELEGATE,%s,%s"), Object == nullptr ? TEXT("NO OBJ") : *Object->GetPathName(), *FunctionNameStr);
			}
			return TEXT("UNBOUND DELEGATE");
		}
	case FFramerDelegateVariant::IndexOfType<FFramerDynamicDelegate>():
		{
			const FFramerDynamicDelegate& FuncDynDelegate = VariantDelegate.Get<FFramerDynamicDelegate>();
			if (FuncDynDelegate.IsBound())
			{
				const UObject* Object = FuncDynDelegate.GetUObject();
				return FString::Printf(TEXT("DYN DELEGATE,%s,%s"), Object == nullptr ? TEXT("NO OBJ") : *Object->GetPathName(), *FuncDynDelegate.GetFunctionName().ToString());
			}
			return TEXT("UNBOUND DYN DELEGATE");
		}
	case FFramerDelegateVariant::IndexOfType<FFramerFunction>():
		{
			if (const FFramerFunction& FramerFunction = VariantDelegate.Get<FFramerFunction>())
			{
				return TEXT("TFUNCTION");
			}
			return TEXT("UNBOUND TFUNCTION");
		}
	default:
		return TEXT("UNSET");
	}
}

FFramerData::FFramerData()
	: Interval(0)
	, Total(0)
	, Remaining(0)
	, RemainingInterval(0)
	, bRequiresDelegate(false)
	, bIsImportant(false)
	, WaitedFrames(0)
	, LevelCollection(ELevelCollectionType::DynamicSourceLevels)
{
	Status.store(EFramerStatus::Invalid, std::memory_order_relaxed);
}

#ifndef UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD
	#define UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD !UE_BUILD_SHIPPING
#endif

#if UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD
static int32 DumpAllFramerLogsThreshold = -1;
static FAutoConsoleVariableRef CVarDumpAllFramerLogsThreshold(
	TEXT("FM.DumpAllFramerLogsThreshold"),
	DumpAllFramerLogsThreshold,
	TEXT("Threshold (in count of active framers) at which to dump info about all active framers to logs. -1 means this is disabled. NOTE: This will only be dumped once per process launch."));
#endif // #if UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD

#if UE_ENABLE_TRACKING_FRAMER_SOURCES
static int32 GBuildFramerSourceList = 0;
static FAutoConsoleVariableRef CVarGBuildFramerSourceList(
	TEXT("FM.BuildFramerSourceList"), GBuildFramerSourceList,
	TEXT("When non-zero, tracks which framers expire each frame, dumping them during shutdown or when the flag is changed back to 0.")
	TEXT("\n  0: Off")
	TEXT("\n  1: On - Group framers by class (useful to focus on entire systems of things, especially bad spikey frames where we care about aggregates)")
	TEXT("\n  2: On - Do not group framers by class (useful if individual instances are problematic)"),
	ECVF_Default);

// Information about a single framer or framer source being tracked
struct FFramerSourceEntry
{
	uint64 TotalCount = 0;
	int32 SumApproxInterval = 0;
	uint32 LastFrameID = 0;

	void UpdateEntry(int32 Interval, uint32 FrameID)
	{
		++TotalCount;
		SumApproxInterval += Interval;

		if (LastFrameID != FrameID)
		{
			LastFrameID = FrameID;
		}
	}
};

// List of information about all framers that have expired during a tracking window
struct FFramerSourceList
{
	TMap<FString, FFramerSourceEntry> Entries;
	UWorld* OwnerWorld = nullptr;

	// This is similar to FFramerUnifiedDelegate::ToString() but it tries to find the base class / exclude vptr printout info so that framers are collapsed/aggregated better
	static FString GetPartialDeduplicateDelegateToString(const FFramerUnifiedDelegate& Delegate)
	{
		FString FunctionNameStr = TEXT("NotBound!");
		FString ObjectNameStr = TEXT("NotBound!");

		if (const FFramerDelegate* FuncDelegate = Delegate.VariantDelegate.TryGet<FFramerDelegate>())
		{
			if (FuncDelegate->IsBound())
			{
				ObjectNameStr = TEXT("NonDynamicDelegate");
				FName FunctionName;
#if USE_DELEGATE_TRYGETBOUNDFUNCTIONNAME
				FunctionName = FuncDelegate->TryGetBoundFunctionName();
#endif
				if (FunctionName.IsNone())
				{
					uint64 ProgramCounter = FuncDelegate->GetBoundProgramCounterForTimerManager();
					if (ProgramCounter != 0)
					{
						// Add the function address
						if (DumpFramerLogSymbolNames)
						{
							// Try to resolve the function address to a symbol
							FProgramCounterSymbolInfo SymbolInfo;
							FPlatformStackWalk::ProgramCounterToSymbolInfo(ProgramCounter, /*out*/ SymbolInfo);
							FunctionNameStr = FString::Printf(TEXT("%s [%s:%d]"), ANSI_TO_TCHAR(SymbolInfo.FunctionName), ANSI_TO_TCHAR(SymbolInfo.Filename), SymbolInfo.LineNumber);
						}
						else
						{
							FunctionNameStr = FString::Printf(TEXT("func: 0x%llx"), ProgramCounter);
						}
					}
					else
					{
						FunctionNameStr = TEXT("0x0");
					}
				}
				else
				{
					FunctionNameStr = FunctionName.ToString();
				}
			}
		}
		else if (const FFramerDynamicDelegate* FuncDynDelegate = Delegate.VariantDelegate.TryGet<FFramerDynamicDelegate>())
		{
			if (FuncDynDelegate->IsBound())
			{
				const FName FuncFName = FuncDynDelegate->GetFunctionName();
				FunctionNameStr = FuncFName.ToString();

				UClass* SourceClass = nullptr;
				if (const UObject* Object = FuncDynDelegate->GetUObject())
				{
					SourceClass = Object->GetClass();
					if (UFunction* Func = SourceClass->FindFunctionByName(FuncFName))
					{
						SourceClass = Func->GetOwnerClass();
					}
				}

				ObjectNameStr = GetPathNameSafe(SourceClass);
			}
		}
		else if (const FFramerFunction* TimerFunction = Delegate.VariantDelegate.TryGet<FFramerFunction>())
		{
			if (*TimerFunction)
			{
				FunctionNameStr = TEXT("TFunction");
				ObjectNameStr = TEXT("");	
			}
		}

		return FString::Printf(TEXT("%s,\"%s\""), *ObjectNameStr, *FunctionNameStr);
	}

	void AddEntry(const FFramerData& Data)
	{
		Entries.FindOrAdd((GBuildFramerSourceList == 2) ? Data.FramerDelegate.ToString() : GetPartialDeduplicateDelegateToString(Data.FramerDelegate)).UpdateEntry(Data.Interval, GFrameCounter);
	}

	void DumpEntries()
	{
		FString AdditionalInfoAboutGameInstance;
		if (OwnerWorld != nullptr)
		{
			if (FWorldContext* WorldContext = OwnerWorld->GetGameInstance() ? OwnerWorld->GetGameInstance()->GetWorldContext() : nullptr)
			{
				if (WorldContext->RunAsDedicated)
				{
					AdditionalInfoAboutGameInstance = TEXT(" (dedicated server)");
				}
			}
		}

		UE_LOG(LogEngine, Log, TEXT("-----------------------"));
		UE_LOG(LogEngine, Log, TEXT("Listing Expired Timer Stats for OwnerWorld=%s%s"), *GetPathNameSafe(OwnerWorld), *AdditionalInfoAboutGameInstance);
		
		UE_LOG(LogEngine, Log, TEXT("TotalExpires,IntervalAvg,FunctionName"));
		Entries.ValueSort([](const FFramerSourceEntry& A, const FFramerSourceEntry& B) { return A.TotalCount > B.TotalCount; });
		for (const auto& KVP : Entries)
		{
			UE_LOG(LogEngine, Log, TEXT("%llu,%llu,%s"), KVP.Value.TotalCount, KVP.Value.SumApproxInterval / KVP.Value.TotalCount, *KVP.Key);
		}

		UE_LOG(LogEngine, Log, TEXT("-----------------------"));
	}

	~FFramerSourceList()
	{
		DumpEntries();
	}
};
#endif // UE_ENABLE_TRACKING_FRAMER_SOURCES

namespace
{
	void DescribeFFramerDataSafely(FOutputDevice& Ar, const FFramerData& Data)
	{
		Ar.Logf(
			TEXT("FramerData %p : Interval=%d, Remaining=%d, Total=%d, bRequiresDelegate=%s, Status=%d, Delegate=%s"),
			&Data,
			Data.Interval,
			Data.Remaining,
			Data.Total,
			Data.bRequiresDelegate ? TEXT("true") : TEXT("false"),
			static_cast<int32>(Data.Status.load(std::memory_order_release)),
			*Data.FramerDelegate.ToString()
		);
	}

	FString GetFFramerDataSafely(const FFramerData& Data)
	{
		FStringOutputDevice Output;
		DescribeFFramerDataSafely(Output, Data);
		return MoveTemp(Output);
	}
}

struct FFramerHeapOrder
{
	explicit FFramerHeapOrder(const TFramerPool<FFramerData>& InFramers)
		: Framers(InFramers)
	{
	}

	bool operator()(FFramerHandle LhsHandle, FFramerHandle RhsHandle) const
	{
		int32 LhsIndex = LhsHandle.GetIndex();
		int32 RhsIndex = RhsHandle.GetIndex();

		const FFramerData& LhsData = Framers[LhsIndex];
		const FFramerData& RhsData = Framers[RhsIndex];

		// PriorityScore = Remaining - WaitedFrames * StarvationFactor
		int32 LhsScore = LhsData.Remaining - LhsData.WaitedFrames * GStarvationFactor;
		int32 RhsScore = RhsData.Remaining - RhsData.WaitedFrames * GStarvationFactor;

		return LhsScore < RhsScore;
	}

	const TFramerPool<FFramerData>& Framers;
};


TMap<UWorld*, FFramerManager*> FFramerManager::FramerManagers;
FDelegateHandle FFramerManager::OnWorldInitHandle;
FDelegateHandle FFramerManager::OnWorldCleanupHandle;

FFramerManager* FFramerManager::Get(UWorld* InWorld)
{
	FFramerManager** OutWorld = FramerManagers.Find(InWorld);
	if (OutWorld == nullptr)
	{
		UE_LOG(LogFramerManager, Warning, TEXT("Calling FFramerManager::Get \"%s\", but Framer has never encountered this world before. "
			" This means that WorldInit never happened. This may happen in some edge cases in the editor, like saving invisible child levels, "
			"in which case the calling context needs to be safe against this returning nullptr."), InWorld ? *InWorld->GetName() : TEXT("nullptr"));
		return nullptr;
	}
	return *OutWorld;
}

FFramerManager* FFramerManager::Get(const UObject* WorldContextObject)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	return FFramerManager::Get(World);
}

void FFramerManager::OnStartup()
{
	OnWorldInitHandle = FWorldDelegates::OnPreWorldInitialization.AddStatic(&FFramerManager::OnWorldInit);
	OnWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(&FFramerManager::OnWorldCleanup);
}

void FFramerManager::OnShutdown()
{
	FWorldDelegates::OnPreWorldInitialization.Remove(OnWorldInitHandle);
	FWorldDelegates::OnWorldCleanup.Remove(OnWorldCleanupHandle);

	//Should have cleared up all world managers by now.
	if (!ensure(FramerManagers.Num() == 0))
	{
		for (TPair<UWorld*, FFramerManager*> Pair : FramerManagers)
		{
			FFramerManager* WorldMan = Pair.Value;
			if (ensure(WorldMan))
			{
				delete WorldMan;
			}

			Pair.Value = nullptr;
		}
		FramerManagers.Empty();
	}
}

bool FFramerManager::IsTickableWhenPaused() const
{
	return GTickWhenWorldPaused;
}

void FFramerManager::Tick(float DeltaSeconds)
{
	SCOPED_NAMED_EVENT(FFramerManager_Tick, FColor::Orange);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FramerManager);

#if DO_TIMEGUARD && 0
	TArray<FFramerUnifiedDelegate> RunFramerDelegates;
	FFramerNameDelegate NameFunction = FFramerNameDelegate::CreateLambda([&] {
			FString ActiveDelegates;
			for ( const FFramerUnifiedDelegate& Descriptor : RunFramerDelegates )
			{
				ActiveDelegates += FString::Printf(TEXT("Delegate %s, "), *Descriptor.ToString() );
			}
			return FString::Printf(TEXT("UWorld::Tick - FramerManager, %s"), *ActiveDelegates);
		});

	// no delegate should take longer then 5ms to run 
	SCOPE_TIME_GUARD_DELEGATE_MS(NameFunction, 5);
#endif	

	// @todo, might need to handle long-running case
	// (e.g. every X seconds, renormalize to InternalTime = 0)

	INC_DWORD_STAT_BY(STAT_NumFramerHeapEntries, ActiveFramerHeap.Num());

	check(IsInGameThread());

	if (HasBeenTickedThisFrame())
	{
		return;
	}

#if UE_ENABLE_TRACKING_FRAMER_SOURCES
	if ((FramerSourceList == nullptr) != (GBuildFramerSourceList == 0))
	{
		if (FramerSourceList == nullptr)
		{
			FramerSourceList.Reset(new FFramerSourceList);
			FramerSourceList->OwnerWorld = OwnerWorld;
		}
		else
		{
			FramerSourceList.Reset();
		}
	}
#endif

#if UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD
	// Dump framer info to logs if we have way too many framers active.
	UE_SUPPRESS(LogEngine, Warning,
	{
		if (DumpAllFramerLogsThreshold > 0 && ActiveFramerHeap.Num() > DumpAllFramerLogsThreshold)
		{
			static bool bAlreadyLogged = false;
			if(!bAlreadyLogged)
			{
				bAlreadyLogged = true;
			
				UE_LOG(LogEngine, Warning, TEXT("Number of active Framers (%d) has exceeded DumpAllFramerLogsThreshold (%d)!  Dumping all framer info to log:"), ActiveFramerHeap.Num(), DumpAllFramerLogsThreshold);

				TArray<const FFramerData*> ValidActiveFramers;
				ValidActiveFramers.Reserve(ActiveFramerHeap.Num());
				for (FFramerHandle Handle : ActiveFramerHeap)
				{
					if (const FFramerData* Data = FindFramer(Handle))
					{
						ValidActiveFramers.Add(Data);
					}
				}

				for (const FFramerData* Data : ValidActiveFramers)
				{
					DescribeFFramerDataSafely(*GLog, *Data);
				}
			}
		}
	});
#endif // #if UE_ENABLE_DUMPALLFRAMERLOGSTHRESHOLD

	ProcessPendingCommands();
	
	// Initialize budget tracking
	const double InitialBudgetMs = GTickBudgetMs > 0 ? GTickBudgetMs : -1.0;
	double RemainingBudgetMs = InitialBudgetMs;
	
	// Track stats for debugging
	int32 ImportantFramerExecutedCount = 0;
	int32 NormalFramerExecutedCount = 0;
	TArray<TPair<FString, double>> ExpensiveFramerInfos;
	
	// Update WaitedFrames for all active non-important framers
	// Important framers will be reset when executed
	for (const FFramerHandle& Handle : ActiveFramerHeap)
	{
		FFramerData* Data = FindFramer(Handle);
		if (Data && Data->Status.load(std::memory_order_acquire) == EFramerStatus::Active && !Data->bIsImportant)
		{
			Data->WaitedFrames++;
		}
	}

	ExecuteFramerHeap(ImportantFramerHeap, ImportantFramerExecutedCount, RemainingBudgetMs, false, ExpensiveFramerInfos);
	double ImportantOverhead = InitialBudgetMs - RemainingBudgetMs;

	ExecuteFramerHeap(ActiveFramerHeap, NormalFramerExecutedCount, RemainingBudgetMs, GTickBudgetMs > 0, ExpensiveFramerInfos);
	
	if (GBudgetDebugEnabled && GTickBudgetMs > 0 && NormalFramerExecutedCount < ActiveFramerHeap.Num())
	{
		UE_LOG(LogFramerManager, Warning, TEXT("FramerManager: Tick over budget! Budget: %.2fms, ImportantOverhead: %.2fms, ExecutedCount: %d, TotalCount: %d"), 
				GTickBudgetMs, ImportantOverhead, NormalFramerExecutedCount, ActiveFramerHeap.Num());
			
		if (GBudgetDebugEnabled && ExpensiveFramerInfos.Num() > 0)
		{
			UE_LOG(LogFramerManager, Log, TEXT("Top expensive framers:"));
			int32 Count = 0;
			for (const auto& ExpensiveInfo : ExpensiveFramerInfos)
			{
				if (Count >= 10) break;
				UE_LOG(LogFramerManager, Log, TEXT("  %d. %s - %.2fms"), Count + 1, *ExpensiveInfo.Key, ExpensiveInfo.Value);
				Count++;
			}
		}
	}

	// Framer has been ticked.
	LastTickedFrame = GFrameCounter;

	// If we have any Pending Framers, add them to the Active Queue.
	if( PendingFramerSet.Num() > 0 )
	{
		for (FFramerHandle Handle : PendingFramerSet)
		{
			if (FFramerData* TimerToActivate = FindFramer(Handle))
			{
				TimerToActivate->Status.store(EFramerStatus::Active, std::memory_order_release);
			
				if (TimerToActivate->bIsImportant)
				{
					ImportantFramerHeap.HeapPush(Handle, FFramerHeapOrder(FramerSlots));
				}
				else
				{
					ActiveFramerHeap.HeapPush(Handle, FFramerHeapOrder(FramerSlots));
				}
			}
		}
		PendingFramerSet.Reset();
	}
}

void FFramerManager::OnWorldInit(UWorld* World, const UWorld::InitializationValues IVS)
{
	FFramerManager*& NewManager = FramerManagers.FindOrAdd(World);
	if (!NewManager)
	{
		NewManager = new FFramerManager();
	}

	NewManager->OwnerWorld = World;
}

void FFramerManager::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	//Cleanup world manager contents but not the manager itself.
	FFramerManager** Manager = FramerManagers.Find(World);
	if (Manager)
	{
		FFramerManager* WorldMan = *Manager;
		if (ensure(WorldMan))
		{
			delete WorldMan;
			FramerManagers.Remove(World);
		}
	}
}

FFramerManager::FFramerManager()
{
	// Pre-allocate framer slots
	FramerSlots.AddChunk();
	FramerSlotsNum.store(0, std::memory_order_relaxed);

	// Push all pre-allocated indices into the free list (in reverse order so index 0 is popped first)
	for (int32 i = FramerSlots.Num() - 1; i >= 0; --i)
	{
		FreeFramers.Push(new uint32(i));
	}
}

FFramerManager::~FFramerManager()
{
	// Drain free list to avoid memory leaks (the uint32* were heap-allocated for the lock-free list)
	uint32* IndexPtr = nullptr;
	while ((IndexPtr = FreeFramers.Pop()) != nullptr)
	{
		delete IndexPtr;
	}
}

void FFramerManager::OnCrash()
{
	check(IsInGameThread());
	
	UE_LOG(LogFramerManager, Warning, TEXT("FramerManager %p on crashing delegate called, dumping extra information"), this);

	UE_LOG(LogFramerManager, Log, TEXT("------- %d Active Framers (including expired) -------"), ActiveFramerHeap.Num());
	int32 ExpiredActiveFramerCount = 0;
	for (FFramerHandle Handle : ActiveFramerHeap)
	{
		if (const FFramerData* Framer = FindFramer(Handle))
		{
			if (Framer->Status.load(std::memory_order_release) == EFramerStatus::ActivePendingRemoval)
			{
				++ExpiredActiveFramerCount;
			}
			else
			{
				DescribeFFramerDataSafely(*GLog, *Framer);
			}
		}
	}
	UE_LOG(LogFramerManager, Log, TEXT("------- %d Expired Active Framers -------"), ExpiredActiveFramerCount);
	
	UE_LOG(LogFramerManager, Log, TEXT("------- %d Paused Framers -------"), PausedFramerSet.Num());
	for (FFramerHandle Handle : PausedFramerSet)
	{
		if (const FFramerData* Framer = FindFramer(Handle))
		{
			DescribeFFramerDataSafely(*GLog, *Framer);
		}
	}
	
	UE_LOG(LogFramerManager, Log, TEXT("------- %d Pending Framers -------"), PendingFramerSet.Num());
	for (FFramerHandle Handle : PendingFramerSet)
	{
		if (const FFramerData* Framer = FindFramer(Handle))
		{
			DescribeFFramerDataSafely(*GLog, *Framer);
		}
	}
	
	UE_LOG(LogFramerManager, Log, TEXT("------- %d Total Framers -------"), PendingFramerSet.Num() + PausedFramerSet.Num() + ActiveFramerHeap.Num() - ExpiredActiveFramerCount);
	
	UE_LOG(LogFramerManager, Warning, TEXT("FramerManager %p dump ended"), this);
}

FFramerHandle FFramerManager::K2_FindDynamicFramerHandle(FFramerDynamicDelegate InDynamicDelegate) const
{
	check(IsInGameThread());
	
	FFramerHandle Result;

	if (const void* Obj = InDynamicDelegate.GetUObject())
	{
		if (const TSet<FFramerHandle>* FramerForObject = ObjectToFramers.Find(Obj))
		{
			for (FFramerHandle Handle : *FramerForObject)
			{
				const FFramerData* Data = FindFramer(Handle);
				if (!Data || Data->Status.load(std::memory_order_acquire) == EFramerStatus::ActivePendingRemoval)
				{
					continue;
				}
				const FFramerDynamicDelegate* DynDelegate = Data->FramerDelegate.VariantDelegate.TryGet<FFramerDynamicDelegate>();
				if (DynDelegate && *DynDelegate == InDynamicDelegate)
				{
					Result = Handle;
					break;
				}
			}
		}
	}

	return Result;
}

void FFramerManager::ListFramers() const
{
	// not currently threadsafe
	check(IsInGameThread());

	TArray<const FFramerData*> ValidActiveFramers;
	ValidActiveFramers.Reserve(ActiveFramerHeap.Num());
	for (FFramerHandle Handle : ActiveFramerHeap)
	{
		if (const FFramerData* Data = FindFramer(Handle))
		{
			ValidActiveFramers.Add(Data);
		}
	}

	UE_LOG(LogFramerManager, Log, TEXT("------- %d Active Timers -------"), ValidActiveFramers.Num());
	for (const FFramerData* Data : ValidActiveFramers)
	{
		check(Data);
		DescribeFFramerDataSafely(*GLog, *Data);
	}

	UE_LOG(LogFramerManager, Log, TEXT("------- %d Paused Timers -------"), PausedFramerSet.Num());
	for (FFramerHandle Handle : PausedFramerSet)
	{
		if (const FFramerData* Data = FindFramer(Handle))
		{
			DescribeFFramerDataSafely(*GLog, *Data);
		}
	}

	UE_LOG(LogFramerManager, Log, TEXT("------- %d Pending Timers -------"), PendingFramerSet.Num());
	for (FFramerHandle Handle : PendingFramerSet)
	{
		if (const FFramerData* Data = FindFramer(Handle))
		{
			DescribeFFramerDataSafely(*GLog, *Data);
		}
	}

	UE_LOG(LogFramerManager, Log, TEXT("------- %d Total Timers -------"), PendingFramerSet.Num() + PausedFramerSet.Num() + ValidActiveFramers.Num());
}

void FFramerManager::ProcessPendingCommands()
{
	while (!CommandQueue.IsEmpty())
	{
		FFramerCommand FramerCommand;
		CommandQueue.Dequeue(FramerCommand);

		switch (FramerCommand.Type)
		{
		case FFramerCommand::EType::Set:
			{
				int32 Index = FramerCommand.Handle.GetIndex();
				int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
				if (Index < 0 || Index >= CurrentNum)
				{
					break;
				}

				FFramerData& Data = FramerSlots[Index];

				// Validate handle serial number and that this is still PendingAdd
				if (Data.Handle != FramerCommand.Handle)
				{
					break;
				}

				EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);
				if (CurrentStatus != EFramerStatus::PendingAdd)
				{
					// Framer was already cleared/removed before we processed the Add
					break;
				}

				// Maintain ObjectToFramers mapping
				if (const void* ObjKey = Data.FramerIndicesByObjectKey)
				{
					TSet<FFramerHandle>& HandleSet = ObjectToFramers.FindOrAdd(ObjKey);
					HandleSet.Add(FramerCommand.Handle);
				}

				Data.Status.store(EFramerStatus::Active, std::memory_order_release);
					
				if (Data.bIsImportant)
				{
					ImportantFramerHeap.HeapPush(FramerCommand.Handle, FFramerHeapOrder(FramerSlots));
				}
				else
				{
					ActiveFramerHeap.HeapPush(FramerCommand.Handle, FFramerHeapOrder(FramerSlots));
				}
				break;
			}
		case FFramerCommand::EType::Clear:
			{
				RemoveFramer(FramerCommand.Handle);
				break;
			}
		case FFramerCommand::EType::ClearByObject:
			{
				InternalClearAllFramers(FramerCommand.Object);
				break;
			}
		case FFramerCommand::EType::Pause:
			{
				int32 Index = FramerCommand.Handle.GetIndex();
				int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
				if (Index < 0 || Index >= CurrentNum)
				{
					break;
				}

				FFramerData& Data = FramerSlots[Index];
				if (Data.Handle != FramerCommand.Handle)
				{
					break;
				}

				// Check if already paused or removed
				EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);
				if (CurrentStatus == EFramerStatus::Paused ||
					CurrentStatus == EFramerStatus::Invalid ||
					CurrentStatus == EFramerStatus::ActivePendingRemoval)
				{
					break;
				}

				EFramerStatus PreviousStatus = CurrentStatus;

				// Remove from previous container
				switch (PreviousStatus)
				{
				case EFramerStatus::Active:
					{
						int32 IndexIndex = ActiveFramerHeap.Find(FramerCommand.Handle);
						if (IndexIndex != INDEX_NONE)
						{
							ActiveFramerHeap.HeapRemoveAt(IndexIndex, FFramerHeapOrder(FramerSlots), EAllowShrinking::No);
						}
					}
					break;

				case EFramerStatus::Pending:
				case EFramerStatus::PendingAdd:
					PendingFramerSet.Remove(FramerCommand.Handle);
					break;

				case EFramerStatus::Executing:
					if (CurrentlyExecutingFramer == FramerCommand.Handle)
					{
						CurrentlyExecutingFramer.Invalidate();
					}
					break;

				default:
					break;
				}

				// Don't pause if currently executing and remaining frame less or equal 0
				if (PreviousStatus == EFramerStatus::Executing && !FramerCanExecute(&Data))
				{
					Data.Status.store(EFramerStatus::Invalid, std::memory_order_release);
					RemoveFramerInternal(FramerCommand.Handle);
				}
				else
				{
					// Add to Paused list
					PausedFramerSet.Add(FramerCommand.Handle);
					Data.Status.store(EFramerStatus::Paused, std::memory_order_release);
				}
				break;
			}
		case FFramerCommand::EType::Unpause:
			{
				int32 Index = FramerCommand.Handle.GetIndex();
				int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
				if (Index < 0 || Index >= CurrentNum)
				{
					break;
				}

				FFramerData& Data = FramerSlots[Index];
				if (Data.Handle != FramerCommand.Handle)
				{
					break;
				}

				// Check if already unpaused or removed
				EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);
				if (CurrentStatus == EFramerStatus::Invalid ||
					CurrentStatus == EFramerStatus::ActivePendingRemoval ||
					CurrentStatus == EFramerStatus::Active ||
					CurrentStatus == EFramerStatus::Pending ||
					CurrentStatus == EFramerStatus::PendingAdd)
				{
					break;
				}

				// Remove from paused list
				PausedFramerSet.Remove(FramerCommand.Handle);

				Data.Status.store(EFramerStatus::Active, std::memory_order_release);
				ActiveFramerHeap.HeapPush(FramerCommand.Handle, FFramerHeapOrder(FramerSlots));
				break;
			}
		default:
			check(false);
		}
	}
}

void FFramerManager::ExecuteFramerHeap(TArray<FFramerHandle>& FramerHeap, int32& OutExecutedCount, double& InOutRemainingBudgetMs, bool bTrackBudget, TArray<TPair<FString, double>>& InOutExpensiveFramerInfos)
{
	while (FramerHeap.Num() > 0)
	{
		if (bTrackBudget && InOutRemainingBudgetMs < 0)
		{
			break;
		}

		FFramerHandle TopHandle;
		FramerHeap.HeapPop(TopHandle, FFramerHeapOrder(FramerSlots), EAllowShrinking::No);

		// Test for expired framers
		int32 TopIndex = TopHandle.GetIndex();
		FFramerData* Top = &FramerSlots[TopIndex];

		if (Top->Status.load(std::memory_order_acquire) == EFramerStatus::ActivePendingRemoval)
		{
			InternalClearFramer(TopHandle);
			continue;
		}

		if (Top->RemainingInterval == 0)
		{
			// Set the relevant level context for this timer
			const int32 LevelCollectionIndex = OwnerWorld ? OwnerWorld->FindCollectionIndexByType(Top->LevelCollection) : INDEX_NONE;
			FScopedLevelCollectionContextSwitch LevelContext(LevelCollectionIndex, OwnerWorld);

			// Store it while we're executing
			CurrentlyExecutingFramer = TopHandle;
			Top->Status.store(EFramerStatus::Executing, std::memory_order_release);

#if UE_ENABLE_TRACKING_FRAMER_SOURCES
			if (FramerSourceList.IsValid())
			{
				FramerSourceList->AddEntry(*Top);
			}
#endif

			// Now call the function
			{
				const double FramerStartTime = bTrackBudget ? FPlatformTime::Seconds() : 0.0;

				checkf(!WillRemoveFramerAssert(CurrentlyExecutingFramer), TEXT("RemoveFramer(CurrentlyExecutingFramer) - due to fail before Execute()"));
				Top->FramerDelegate.Execute();
				Top->WaitedFrames = 0;
				--Top->Remaining;

				// Track time spent
				const double ElapsedMs = bTrackBudget ? (FPlatformTime::Seconds() - FramerStartTime) * 1000.0 : 0.0;
				if (GTickBudgetMs > 0)
				{
					InOutRemainingBudgetMs -= ElapsedMs;
				}
				OutExecutedCount++;

				// Track expensive framers for debugging (top 10 by execution time)
				if (GBudgetDebugEnabled && GTickBudgetMs > 0 && ElapsedMs > 0)
				{
					FString FramerInfo = Top->FramerDelegate.ToString();
					InOutExpensiveFramerInfos.Add(TPair<FString, double>(FramerInfo, ElapsedMs));
					
					// Keep only top 10 most expensive
					if (InOutExpensiveFramerInfos.Num() > 10)
					{
						InOutExpensiveFramerInfos.Sort([](const auto& A, const auto& B) {
							return A.Value > B.Value; // Descending order
						});
						InOutExpensiveFramerInfos.SetNum(10);
					}
				}
			}

			// Test to ensure it didn't get cleared during execution
			if (Top)
			{
				// If framer requires a delegate, make sure it's still validly bound
				if (FramerCanExecute(Top) && (!Top->bRequiresDelegate || Top->FramerDelegate.IsBound()))
				{
					// Put this framer back to pending
					Top->RemainingInterval = Top->Interval - 1;
					Top->Status.store(EFramerStatus::Pending, std::memory_order_release);
					PendingFramerSet.Add(TopHandle);
				}
				else
				{
					InternalClearFramer(CurrentlyExecutingFramer);
					CurrentlyExecutingFramer.Invalidate();
				}
			}
		}
		else
		{
			// RemainingInterval > 0, just decrement
			if (Top && FramerCanExecute(Top) && Top->RemainingInterval > 0)
			{
				--Top->RemainingInterval;
				Top->Status.store(EFramerStatus::Pending, std::memory_order_release);
				PendingFramerSet.Add(TopHandle);
			}
		}
	}
}

void FFramerManager::InternalSetFramer(FFramerHandle& InOutHandle, FFramerUnifiedDelegate&& InDelegate, int32 InInterval, int32 InTotal, int32 InFirstDelay, bool bInIsImportant)
{
	SCOPE_CYCLE_COUNTER(STAT_SetFramer);

#if UE_WITH_REMOTE_OBJECT_HANDLE && UE_AUTORTFM
	ensureAlwaysMsgf(!InDelegate.VariantDelegate.IsType<FTimerFunction>(), TEXT("Non-UObject Delegates (TFunctions) are not safe with DSTM. Use FTimerDelegate::CreateUObject and don't capture raw pointers."));
#endif

	if (InOutHandle.IsValid())
	{
		FFramerData* ExistingFramer = FindFramerThreadSafe(InOutHandle);
		if (ExistingFramer)
		{
			ExistingFramer->Status.store(EFramerStatus::PendingRemoval, std::memory_order_release);
			InternalClearFramer(InOutHandle);
		}
	}

	if (InInterval <= 0)
	{
		InOutHandle.Invalidate();
		return;
	}

	int32 NewIndex = AllocateFramerSlot();

	FFramerData& NewFramerData = FramerSlots[NewIndex];
	NewFramerData.FramerDelegate = MoveTemp(InDelegate);

	NewFramerData.Interval = InInterval;
	NewFramerData.Total = InTotal;
	NewFramerData.bRequiresDelegate = NewFramerData.FramerDelegate.IsBound();
	NewFramerData.FramerIndicesByObjectKey = NewFramerData.FramerDelegate.GetBoundObject();
	NewFramerData.bIsImportant = bInIsImportant;
	NewFramerData.WaitedFrames = 0;

	// Set level collection (only safe to access on game thread, but we set a reasonable default)
	if (OwnerWorld && OwnerWorld->GetActiveLevelCollection())
	{
		NewFramerData.LevelCollection = OwnerWorld->GetActiveLevelCollection()->GetType();
	}
	else
	{
		NewFramerData.LevelCollection = ELevelCollectionType::DynamicSourceLevels;
	}

	// Write Remaining and RemainingInterval under FramerFieldLock so cross-thread readers see consistent values
	{
		UE::TWriteScopeLock Lock(FramerFieldLock);
		NewFramerData.Remaining = InTotal;
		NewFramerData.RemainingInterval = (InFirstDelay > 0 ? InFirstDelay : InInterval) - 1;
	}

	FFramerHandle NewFramerHandle = GenerateHandle(NewIndex);
	NewFramerData.Handle = NewFramerHandle;
	NewFramerData.Status.store(EFramerStatus::PendingAdd, std::memory_order_release);

	FFramerCommand AddCmd;
	AddCmd.Type = FFramerCommand::EType::Set;
	AddCmd.Handle = NewFramerHandle;
	CommandQueue.Enqueue(AddCmd);

	InOutHandle = NewFramerHandle;
}

void FFramerManager::InternalClearFramer(FFramerHandle InHandle)
{
	FFramerCommand FramerCommand;
	FramerCommand.Type = FFramerCommand::EType::Clear;
	FramerCommand.Handle = InHandle;

	CommandQueue.Enqueue(MoveTemp(FramerCommand));
}

void FFramerManager::InternalClearAllFramers(void const* Object)
{
	SCOPE_CYCLE_COUNTER(STAT_ClearAllFramers);

	if (!Object)
	{
		return;
	}

	TSet<FFramerHandle>* FramersToRemove = ObjectToFramers.Find(Object);
	if (!FramersToRemove)
	{
		return;
	}

	TSet<FFramerHandle> LocalFramersToRemove = *FramersToRemove;
	for (FFramerHandle FramerToRemove : LocalFramersToRemove)
	{
		RemoveFramer(FramerToRemove);
	}
}

void FFramerManager::RemoveFramer(FFramerHandle const& InHandle)
{
	check(IsInGameThread());
	
	int32 Index = InHandle.GetIndex();
	int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
	if (Index < 0 || Index >= CurrentNum)
	{
		return;
	}

	FFramerData& Data = FramerSlots[Index];

	if (Data.Handle != InHandle)
	{
		return;
	}

	EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);
	if (CurrentStatus == EFramerStatus::Invalid)
	{
		return;
	}

	switch (CurrentStatus)
	{
	case EFramerStatus::Pending:
	case EFramerStatus::PendingAdd:
		PendingFramerSet.Remove(InHandle);
		Data.Status.store(EFramerStatus::Invalid, std::memory_order_release);
		RemoveFramerInternal(InHandle);
		break;

	case EFramerStatus::Active:
		// Mark for lazy removal from heap
		Data.Status.store(EFramerStatus::ActivePendingRemoval, std::memory_order_release);
		break;

	case EFramerStatus::ActivePendingRemoval:
		// Already pending removal
		break;

	case EFramerStatus::Paused:
		PausedFramerSet.Remove(InHandle);
		Data.Status.store(EFramerStatus::Invalid, std::memory_order_release);
		RemoveFramerInternal(InHandle);
		break;

	case EFramerStatus::Executing:
		// Being executed right now — clear CurrentlyExecutingFramer to prevent re-push
		if (CurrentlyExecutingFramer == InHandle)
		{
			CurrentlyExecutingFramer.Invalidate();
		}
		Data.Status.store(EFramerStatus::Invalid, std::memory_order_release);
		RemoveFramerInternal(InHandle);
		break;

	default:
		// PendingRemoval, PendingPause, PendingUnpause, Invalid — handle gracefully
		Data.Status.store(EFramerStatus::Invalid, std::memory_order_release);
		RemoveFramerInternal(InHandle);
		break;
	}
}

void FFramerManager::RemoveFramerInternal(FFramerHandle const& Handle)
{
	check(IsInGameThread());
	
	int32 Index = Handle.GetIndex();
	if (Index < 0 || Index >= FramerSlots.Num())
	{
		return;
	}

	FFramerData& Data = FramerSlots[Index];

	// Remove FramerIndicesByObject entry if necessary
	if (const void* FramerIndicesByObjectKey = Data.FramerIndicesByObjectKey)
	{
		TSet<FFramerHandle>* FramersForObject = ObjectToFramers.Find(FramerIndicesByObjectKey);
		if (FramersForObject)
		{
			FramersForObject->Remove(Handle);
			if (FramersForObject->Num() == 0)
			{
				ObjectToFramers.Remove(FramerIndicesByObjectKey);
			}
		}
	}

	// Recycle the slot
	RecycleFramerSlot(Index);
}

bool FFramerManager::WillRemoveFramerAssert(FFramerHandle const& Handle) const
{
	const FFramerData* Data = FindFramer(Handle);

	// Remove TimerIndicesByObject entry if necessary
	if (const void* FramerIndicesByObjectKey = Data ? Data->FramerIndicesByObjectKey : nullptr)
	{
		const TSet<FFramerHandle>* FramersForObject = ObjectToFramers.Find(FramerIndicesByObjectKey);
		if (!FramersForObject)
		{
			return true;
		}

		const FFramerHandle* Found = FramersForObject->Find(Handle);
		if (!Found)
		{
			return true;
		}
	}

	return false;
}

FFramerData* FFramerManager::FindFramer(FFramerHandle const& InHandle)
{
	if (!InHandle.IsValid())
	{
		return nullptr;
	}

	int32 Index = InHandle.GetIndex();
	int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
	if (Index < 0 || Index >= CurrentNum)
	{
		return nullptr;
	}

	FFramerData& Framer = FramerSlots[Index];

	if (Framer.Handle != InHandle || Framer.Status.load(std::memory_order_acquire) == EFramerStatus::ActivePendingRemoval)
	{
		return nullptr;
	}

	return &Framer;
}

FFramerData* FFramerManager::FindFramerThreadSafe(FFramerHandle const& InHandle)
{
	if (!InHandle.IsValid())
	{
		return nullptr;
	}

	int32 Index = InHandle.GetIndex();

	// Atomic range check — no lock needed
	int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
	if (Index < 0 || Index >= CurrentNum)
	{
		return nullptr;
	}

	FFramerData& Framer = FramerSlots[Index];

	EFramerStatus CurrentStatus = Framer.Status.load(std::memory_order_acquire);
	if (CurrentStatus == EFramerStatus::Invalid ||
		CurrentStatus == EFramerStatus::PendingRemoval ||
		CurrentStatus == EFramerStatus::ActivePendingRemoval)
	{
		return nullptr;
	}

	if (Framer.Handle != InHandle)
	{
		return nullptr;
	}

	return &Framer;
}

FFramerHandle FFramerManager::GenerateHandle(int32 Index)
{
	uint64 NewSerialNumber = LastAssignedSerialNumber.fetch_add(1, std::memory_order_relaxed) + 1;
	if (!ensureMsgf(NewSerialNumber != FFramerHandle::MaxSerialNumber, TEXT("Framer serial number has wrapped around!")))
	{
		NewSerialNumber = (uint64)1;
		LastAssignedSerialNumber.store(1, std::memory_order_relaxed);
	}

	FFramerHandle Result;
	Result.SetIndexAndSerialNumber(Index, NewSerialNumber);
	return Result;
}

bool FFramerManager::FramerCanExecute(FFramerData const* const FramerData) const
{
	if (FramerData && FramerData->Handle.IsValid())
	{
		return FramerData->Total == -1 || FramerData->Remaining > 0;
	}
	return false;
}

int32 FFramerManager::AllocateFramerSlot()
{
	// Try to pop from lock-free free list first
	uint32* IndexPtr = FreeFramers.Pop();
	if (IndexPtr)
	{
		int32 Index = *IndexPtr;
		delete IndexPtr;

		int32 CurrentMax = FramerSlotsNum.load(std::memory_order_relaxed);
		while (Index >= CurrentMax)
		{
			if (FramerSlotsNum.compare_exchange_weak(CurrentMax, Index + 1, std::memory_order_release, std::memory_order_relaxed))
			{
				break;
			}
		}

		return Index;
	}

	// Free list exhausted — expand Framers under lock
	{
		FScopeLock Lock(&FramersExpandLock);

		// Double-check: another thread may have expanded while we waited for the lock
		IndexPtr = FreeFramers.Pop();
		if (IndexPtr)
		{
			int32 Index = *IndexPtr;
			delete IndexPtr;

			int32 CurrentMax = FramerSlotsNum.load(std::memory_order_relaxed);
			while (Index >= CurrentMax)
			{
				if (FramerSlotsNum.compare_exchange_weak(CurrentMax, Index + 1, std::memory_order_release, std::memory_order_relaxed))
				{
					break;
				}
			}

			return Index;
		}

		// Actually expand
		int32 OldSize = FramerSlots.Num();
		FramerSlots.AddChunk();
		int32 NewSize = FramerSlots.Num();

		for (int32 i = NewSize - 1; i > OldSize; --i)
		{
			FreeFramers.Push(new uint32(i));
		}

		int32 Index = OldSize;
		int32 CurrentMax = FramerSlotsNum.load(std::memory_order_relaxed);
		while (Index >= CurrentMax)
		{
			if (FramerSlotsNum.compare_exchange_weak(CurrentMax, Index + 1, std::memory_order_release, std::memory_order_relaxed))
			{
				break;
			}
		}

		return Index;
	}
}

void FFramerManager::RecycleFramerSlot(int32 Index)
{
	FramerSlots[Index].Status.store(EFramerStatus::Invalid, std::memory_order_release);
	FramerSlots[Index].Handle.Invalidate();
	FramerSlots[Index].FramerDelegate.Unbind();
	FramerSlots[Index].FramerIndicesByObjectKey = nullptr;

	FreeFramers.Push(new uint32(Index));
}

void FFramerManager::ClearFramer(FFramerHandle& InHandle)
{
	SCOPE_CYCLE_COUNTER(STAT_ClearFramer);
	if (!FramerExists(InHandle))
	{
		return;
	}
	
	if (FFramerData* FramerData = FindFramerThreadSafe(InHandle))
	{
		FramerData->Status.store(EFramerStatus::PendingRemoval, std::memory_order_release);
		InternalClearFramer(InHandle);
	}
	InHandle.Invalidate();
}

void FFramerManager::ClearAllFramersForObject(void const* Object)
{
	if (!Object)
	{
		return;
	}

	FFramerCommand FramerCommand;
	FramerCommand.Type = FFramerCommand::EType::ClearByObject;
	FramerCommand.Object = Object;
	CommandQueue.Enqueue(FramerCommand);
}

bool FFramerManager::FramerExists(FFramerHandle InHandle) const
{
	if (!InHandle.IsValid())
	{
		return false;
	}

	int32 Index = InHandle.GetIndex();
	int32 CurrentNum = FramerSlotsNum.load(std::memory_order_acquire);
	if (Index < 0 || Index >= CurrentNum)
	{
		return false;
	}

	const FFramerData& Data = FramerSlots[Index];
	
	EFramerStatus Status = Data.Status.load(std::memory_order_acquire);
	if (Status == EFramerStatus::Invalid ||
		Status == EFramerStatus::PendingRemoval ||
		Status == EFramerStatus::ActivePendingRemoval)
	{
		return false;
	}

	if (Data.Handle != InHandle)
	{
		return false;
	}

	return true;
}

void FFramerManager::PauseFramer(FFramerHandle InHandle)
{
	if (!FramerExists(InHandle))
	{
		return;
	}

	int32 Index = InHandle.GetIndex();
	FFramerData& Data = FramerSlots[Index];
	EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);

	if (CurrentStatus == EFramerStatus::Paused || CurrentStatus == EFramerStatus::PendingPause)
	{
		return;
	}

	Data.Status.store(EFramerStatus::PendingPause, std::memory_order_release);

	FFramerCommand PauseCmd;
	PauseCmd.Type = FFramerCommand::EType::Pause;
	PauseCmd.Handle = InHandle;
	CommandQueue.Enqueue(PauseCmd);
}

void FFramerManager::UnPauseFramer(FFramerHandle InHandle)
{
	if (!FramerExists(InHandle))
	{
		return;
	}

	int32 Index = InHandle.GetIndex();
	FFramerData& Data = FramerSlots[Index];
	EFramerStatus CurrentStatus = Data.Status.load(std::memory_order_acquire);

	if (CurrentStatus != EFramerStatus::Paused && CurrentStatus != EFramerStatus::PendingPause)
	{
		return;
	}

	Data.Status.store(EFramerStatus::PendingUnpause, std::memory_order_release);

	FFramerCommand UnpauseCmd;
	UnpauseCmd.Type = FFramerCommand::EType::Unpause;
	UnpauseCmd.Handle = InHandle;
	CommandQueue.Enqueue(UnpauseCmd);
}

int32 FFramerManager::GetFramerInterval(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return -1;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];

	UE::TReadScopeLock Lock(FramerFieldLock);
	return Data.Interval;
}

bool FFramerManager::IsFramerActive(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return false;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];
	EFramerStatus Status = Data.Status.load(std::memory_order_acquire);

	switch (Status)
	{
	case EFramerStatus::Active:
	case EFramerStatus::PendingAdd:
	case EFramerStatus::Executing:
	case EFramerStatus::PendingUnpause:
	case EFramerStatus::Pending:
		return true;
	default:
		return false;
	}
}

bool FFramerManager::IsFramerPaused(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return false;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];
	EFramerStatus Status = Data.Status.load(std::memory_order_acquire);

	return (Status == EFramerStatus::Paused || Status == EFramerStatus::PendingPause);
}

bool FFramerManager::IsFramerPending(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return false;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];
	EFramerStatus Status = Data.Status.load(std::memory_order_acquire);

	return (Status == EFramerStatus::Pending || Status == EFramerStatus::PendingAdd);
}

int32 FFramerManager::GetFramerElapsed(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return -1;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];

	UE::TReadScopeLock Lock(FramerFieldLock);
	return Data.Total - Data.Remaining;
}

int32 FFramerManager::GetFramerRemaining(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return -1;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];

	UE::TReadScopeLock Lock(FramerFieldLock);
	return Data.Remaining;
}

int32 FFramerManager::GetTimerTotal(FFramerHandle InHandle) const
{
	if (!FramerExists(InHandle))
	{
		return 0;
	}

	int32 Index = InHandle.GetIndex();
	const FFramerData& Data = FramerSlots[Index];

	return Data.Total;
}
