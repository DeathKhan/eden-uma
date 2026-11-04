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
