// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/kernel/k_hardware_timer_base.h"

namespace Core::Timing {
struct EventType;
} // namespace Core::Timing

namespace Kernel {

// PERF: svc being executed by this host thread (0 outside svcs), for timer statistics.
extern thread_local u32 g_perf_svc;
void PerfNoteTimerTask(s64 delta_ns);

class KHardwareTimer : /* public KInterruptTask, */ public KHardwareTimerBase {
public:
    explicit KHardwareTimer(KernelCore& kernel) : KHardwareTimerBase{kernel} {}

    // Public API.
    void Initialize();
    void Finalize();

    s64 GetTick() const;

    void RegisterAbsoluteTask(KTimerTask* task, s64 task_time) {
        KScopedDisableDispatch dd{m_kernel};
        KScopedSpinLock lk{this->GetLock()};

        PerfNoteTimerTask(task_time - this->GetTick());
        if (this->RegisterAbsoluteTaskImpl(task, task_time)) {
            if (task_time <= m_wakeup_time) {
                this->EnableInterrupt(task_time);
            }
        }
    }

private:
    void EnableInterrupt(s64 wakeup_time);
    void DisableInterrupt();
    bool GetInterruptEnabled();
    void DoTask();

private:
    // Absolute time in nanoseconds
    s64 m_wakeup_time{(std::numeric_limits<s64>::max)()};
    std::shared_ptr<Core::Timing::EventType> m_event_type{};
};

} // namespace Kernel
