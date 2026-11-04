// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/physical_core.h"

namespace Kernel {

thread_local u32 g_perf_svc = 0;

namespace {
// Buckets: <=0, <10us, <100us, <1ms, <10ms, >=10ms
constexpr std::array<s64, 5> PerfBucketLimits{1, 10'000, 100'000, 1'000'000, 10'000'000};
std::array<std::array<std::atomic<u32>, 6>, 0x80> perf_tasks{};
std::atomic<u32> perf_fires{0};
std::atomic<s64> perf_next_log{0};

void PerfMaybeLog(s64 now) {
    s64 next = perf_next_log.load(std::memory_order_relaxed);
    if (now < next || !perf_next_log.compare_exchange_strong(next, now + 20'000'000'000LL)) {
        return;
    }
    if (next == 0) {
        return; // first call only arms the period
    }
    LOG_INFO(Kernel, "PERF timer fires {} in 20 s", perf_fires.exchange(0));
    for (u32 svc = 0; svc < perf_tasks.size(); ++svc) {
        std::array<u32, 6> n{};
        u32 total = 0;
        for (size_t b = 0; b < n.size(); ++b) {
            n[b] = perf_tasks[svc][b].exchange(0, std::memory_order_relaxed);
            total += n[b];
        }
        if (total >= 1000) {
            LOG_INFO(Kernel,
                     "PERF timer svc {:#04x}: {} tasks (<=0 {}, <10us {}, <100us {}, <1ms {}, "
                     "<10ms {}, >=10ms {})",
                     svc, total, n[0], n[1], n[2], n[3], n[4], n[5]);
        }
    }
}
} // namespace

void PerfNoteTimerTask(s64 delta_ns) {
    size_t b = 0;
    while (b < PerfBucketLimits.size() && delta_ns >= PerfBucketLimits[b]) {
        ++b;
    }
    perf_tasks[g_perf_svc & 0x7f][b].fetch_add(1, std::memory_order_relaxed);
}

void KHardwareTimer::Initialize() {
    // Create the timing callback to register with CoreTiming.
    m_event_type = Core::Timing::CreateEvent("KHardwareTimer::Callback",
                                             [this](s64, std::chrono::nanoseconds) {
                                                 this->DoTask();
                                                 return std::nullopt;
                                             });
}

void KHardwareTimer::Finalize() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
    m_event_type.reset();
}

void KHardwareTimer::DoTask() {
    perf_fires.fetch_add(1, std::memory_order_relaxed);
    PerfMaybeLog(GetTick());
    // Handle the interrupt.
    {
        KScopedSchedulerLock slk{m_kernel};
        KScopedSpinLock lk(this->GetLock());

        //! Ignore this event if needed.
        if (!this->GetInterruptEnabled()) {
            return;
        }

        // Disable the timer interrupt while we handle this.
        // Not necessary due to core timing already having popped this event to call it.
        // this->DisableInterrupt();
        m_wakeup_time = (std::numeric_limits<s64>::max)();

        if (const s64 next_time = this->DoInterruptTaskImpl(GetTick());
            0 < next_time && next_time <= m_wakeup_time) {
            // We have a next time, so we should set the time to interrupt and turn the interrupt
            // on.
            this->EnableInterrupt(next_time);
        }
    }

    // Clear the timer interrupt.
    // Kernel::GetInterruptManager().ClearInterrupt(KInterruptName_NonSecurePhysicalTimer,
    //                                              GetCurrentCoreId());
}

bool KHardwareTimer::IdleUntilLocalTask(PhysicalCore& core, s32 core_id) {
    // Short guest timeouts (polling loops; Kirby and the Forgotten Land runs thousands of
    // condition variable waits of tens of microseconds per second) otherwise end with HostTiming
    // futex-waking this idle core thread from another thread, after rescheduling. When the next
    // task belongs to a thread of this core, the core can wait for it itself (a timed wait that
    // the host timer ends) and handle it here. Interrupts still end the wait early. HostTiming
    // keeps its event as a fallback and finds nothing left to do.
    static constexpr s64 LocalWindowNs = 1'000'000;
    s64 task_time;
    KTimerTask* task;
    {
        KScopedSpinLock lk(this->GetLock());
        // KThread is the only kind of timer task.
        task = this->GetNextTask();
        if (task == nullptr || static_cast<KThread*>(task)->GetActiveCore() != core_id) {
            return false;
        }
        task_time = task->GetTime();
    }
    const s64 delta = task_time - this->GetTick();
    if (delta > LocalWindowNs) {
        return false;
    }
    if (delta > 0) {
        core.IdleFor(std::chrono::nanoseconds(delta));
    }
    if (!core.IsInterrupted() && this->GetTick() >= task_time) {
        this->DoTask();
        // If the task is somehow still pending, idle normally rather than spin on it.
        KScopedSpinLock lk(this->GetLock());
        if (this->GetNextTask() == task && task->GetTime() == task_time) {
            return false;
        }
    }
    return true;
}

void KHardwareTimer::EnableInterrupt(s64 wakeup_time) {
    this->DisableInterrupt();

    m_wakeup_time = wakeup_time;
    m_kernel.System().CoreTiming().ScheduleEvent(std::chrono::nanoseconds{m_wakeup_time},
                                                 m_event_type, true);
}

void KHardwareTimer::DisableInterrupt() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type,
                                                   Core::Timing::UnscheduleEventType::NoWait);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
}

s64 KHardwareTimer::GetTick() const {
    return m_kernel.System().CoreTiming().GetGlobalTimeNs().count();
}

bool KHardwareTimer::GetInterruptEnabled() {
    return m_wakeup_time != (std::numeric_limits<s64>::max)();
}

} // namespace Kernel
