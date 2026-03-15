# FramerManager

## 一、概述

FramerManager是UE5.7的一个插件模块，专门用于管理基于帧（Frame）的定时回调，类似于引擎内置的 TimerManager，但专注于帧级精度而非时间精度。可通过时间分摊减少性能尖刺，提升游戏帧率。

### 核心设计目标

*   **帧级精度**：基于帧计数而非时间，执行时机更加可控
*   **线程安全**：支持从任意线程调用 SetFramer/ClearFramer 等操作
*   **预算控制**：内置每帧执行预算，防止过多回调导致帧时间超标(默认不设预算)
*   **优先级调度**：支持 Important 帧任务，保证关键逻辑优先执行
*   **简单易用**：公开接口与TimerManager保持一致，可轻松使用

### 执行时机

FramerManager在紧随TimerManager的TickableGameObjects Time期间执行，示例图如下：

![image.png](Resources\FramerManager.jpg)

***

## 二、与 TimerManager 的对比

### 2.1 核心差异

| 特性           | TimerManager | FramerManager |
| ------------ | ------------ | ------------- |
| 精度基准         | 时间（秒）        | 帧数            |
| 线程安全         | 仅游戏线程        | 任意线程          |
| 预算控制         | 不支持          | 可配置           |
| Important 机制 | 不支持          | 支持            |
| 饥饿预防         | --          | 基于等待帧数        |
| 内存管理         | TSparseArray | TFramerPool   |

### 2.2 使用场景选择

**使用 TimerManager**：

*   需要精确时间控制（如倒计时、延迟）
*   与真实时间相关的逻辑（动画时长、CD时间）
*   帧率无关的逻辑

**使用 FramerManager**：

*   需要帧级精度的逻辑（如每5帧检测一次）
*   多线程环境下需要安全的定时回调
*   需要控制每帧回调执行时间预算
*   关键逻辑需要优先执行保证

***

## 三、核心概念

### 3.1 关键数据结构

```cpp
// FFramerData - 存储单个帧任务的状态
struct FFramerData
{
    int32 Interval;              // 执行间隔（帧数）
    int32 Total;                 // 总执行次数（-1表示无限）
    int32 Remaining;             // 剩余执行次数
    int32 RemainingInterval;     // 距离下次执行的帧数
    int32 WaitedFrames;          // 等待帧数（用于优先级计算）
    uint8 bIsImportant : 1;      // 是否为重要任务
    std::atomic<EFramerStatus> Status;  // 原子状态
    FFramerUnifiedDelegate FramerDelegate;  // 回调委托
    FFramerHandle Handle;         // 句柄
};
```

### 3.2 状态机

```cpp
enum class EFramerStatus : uint16
{
    Pending,                 // 等待执行
    Active,                  // 活跃/可执行
    Paused,                  // 已暂停
    Executing,               // 正在执行
    ActivePendingRemoval,    // 待删除
    // --- 线程安全扩展 ---
    PendingAdd,              // 待添加
    PendingRemoval,          // 待删除
    PendingPause,            // 待暂停
    PendingUnpause,          // 待恢复
    Invalid,                 // 空闲槽位
};
```

***

## 四、核心特性详解

### 4.1 线程安全设计

FramerManager 实现了完整的线程安全保障：

#### MPSC 命令队列

```cpp
// 多生产者单消费者队列
TQueue<FFramerCommand, EQueueMode::Mpsc> CommandQueue;

// 命令类型
struct FFramerCommand
{
    enum class EType { Set, Clear, Pause, Unpause, ClearByObject };
    EType Type;
    FFramerHandle Handle;
    const void* Object;  // 用于 ClearByObject
};

// Tick时先处理本帧命令
void FFramerManager::ProcessPendingCommands()
{
	while (!CommandQueue.IsEmpty())
	{
		FFramerCommand FramerCommand;
		CommandQueue.Dequeue(FramerCommand);

		switch (FramerCommand.Type)
		{
		case FFramerCommand::EType::Set:
		case FFramerCommand::EType::Clear:
		case FFramerCommand::EType::ClearByObject:
		case FFramerCommand::EType::Pause:
		case FFramerCommand::EType::Unpause:
		}
	}
}
```

**工作原理**：

1.  任何线程调用 SetFramer/ClearFramer 时，命令被 Enqueue 到 MPSC 队列
2.  游戏线程在 Tick() 中通过 ProcessPendingCommands() 消费队列
3.  回调始终在Game线程执行

#### lock-free 数据结构

```cpp
// 地址稳定的对象池
TFramerPool<FFramerData> FramerSlots;

// 无锁空闲列表
TLockFreePointerListLIFO<uint32> FreeFramers;
```

**工作原理**：

1. Framer对象池只扩容并保持地址稳定
2. 空闲列表指向对象池中空闲元素下标
3. 无锁结构可以保证线程安全的同时有较高运行效率

#### 锁保护

```cpp
// 对象池扩容保护
FCriticalSection FramersExpandLock;

int32 FFramerManager::AllocateFramerSlot()
{
    {
		FScopeLock Lock(&FramersExpandLock);
		FramerSlots.AddChunk();
	}
}

// 保护频繁读写的轻量字段
mutable UE::FRWSpinLock FramerFieldLock;

int32 FFramerManager::GetFramerInterval(FFramerHandle InHandle) const
{
	UE::TReadScopeLock Lock(FramerFieldLock);
	return Data.Interval;
}
```

**工作原理**：

1. 耗时的扩容操作使用临界区保护，可以有效让出CPU
2. 轻量字段读写采用SpinLock效率更高

### 3.2 预算控制（Budget Control）

```cpp
// 控制台变量
static float GTickBudgetMs = -1.0f;  // 每帧预算（毫秒），-1表示无限制
static int32 GStarvationFactor = 1;   // 饥饿因子，用于优先级计算

// 执行时检查预算
void FFramerManager::ExecuteFramerHeap(...)
{
    while (FramerHeap.Num() > 0)
    {
        if (bTrackBudget && InOutRemainingBudgetMs < 0)
        {
            break;  // 预算耗尽，停止执行
        }
        // ... 执行回调并扣除预算
    }
}
```

**优先级计算**：

```cpp
// PriorityScore = Remaining - WaitedFrames * StarvationFactor
int32 Score = FramerData.Remaining - FramerData.WaitedFrames * GStarvationFactor;
```

Score越小，优先级越高。这确保了等待久的任务优先执行，防止饥饿。

### 3.3 重要任务（Important Framer）

重要任务具有以下特性：

*   始终在本帧执行，不受预算限制
*   优先于普通任务执行
*   适用于关键游戏逻辑（如网络同步、输入处理）

```cpp
// 创建重要任务
MyFramerManager->SetFramer(Handle, this, &UMyClass::CriticalUpdate, 1, -1, 0, true);
```

***

## 五、API 参考

### 5.1 获取实例

```cpp
// 通过 UWorld 获取
FFramerManager* Manager = FFramerManager::Get(UWorld* InWorld);

// 通过 UObject 获取
FFramerManager* Manager = FFramerManager::Get(const UObject* WorldContextObject);
```

### 5.2 设置分帧任务

```cpp
// 模板版本 - UObject 方法
FFramerHandle MyFramerHandle;

FramerManager->SetFramer(
    MyFramerHandle,                    // InOutHandle
    this,                              // InObj - UObject 实例
    &UMyClass::OnFramerTick,          // InFramerMethod - 回调方法
    30,                                // InInterval - 每30帧执行一次
    -1,                                // InTotal - -1表示无限循环
    0,                                 // InFirstDelay - 首次延迟（帧）
    false                              // bInIsImportant - 是否重要
);

// 动态委托版本（适用于 Blueprint）
FramerManager->SetFramer(Handle, MyDynamicDelegate, 30, -1);

// TFunction 版本
FramerManager->SetFramer(Handle, [this]() { /* 回调逻辑 */ }, 30, -1);

// 无委托版本（纯内部计数）
FramerManager->SetFramer(Handle, 30, -1);
```

### 5.3 控制分帧任务

```cpp
// 清除
FramerManager->ClearFramer(MyFramerHandle);

// 暂停/恢复
FramerManager->PauseFramer(MyFramerHandle);
FramerManager->UnPauseFramer(MyFramerHandle);

// 清除对象关联的所有帧任务
FramerManager->ClearAllFramersForObject(MyObject);
```

### 5.4 查询状态

```cpp
// 检查是否存在
if (FramerManager->FramerExists(MyFramerHandle)) { ... }

// 查询状态
bool bActive   = FramerManager->IsFramerActive(MyFramerHandle);
bool bPaused   = FramerManager->IsFramerPaused(MyFramerHandle);
bool bPending  = FramerManager->IsFramerPending(MyFramerHandle);

// 查询参数
int32 Interval   = FramerManager->GetFramerInterval(MyFramerHandle);   // 间隔帧数
int32 Elapsed    = FramerManager->GetFramerElapsed(MyFramerHandle);   // 已执行次数
int32 Remaining  = FramerManager->GetFramerRemaining(MyFramerHandle); // 剩余次数
int32 Total      = FramerManager->GetTimerTotal(MyFramerHandle);      // 总次数
```

### 5.5 调试功能

```cpp
// 列出所有分帧任务
FramerManager->ListFramers();
```

### 5.6 控制台命令

```cpp
// 启用暂停时仍执行（默认 false）
FM.Tick.EnableWhenWorldPaused 1

// 设置帧预算（毫秒）
FM.Tick.BudgetMs 2.0

// 启用预算调试日志
FM.Tick.BudgetDebugEnabled 1

// 帧任务执行时间阈值日志
FM.DumpFramerLogsThreshold 5.0

// 跟踪帧任务来源（非发布版本）
FM.BuildFramerSourceList 1
```
