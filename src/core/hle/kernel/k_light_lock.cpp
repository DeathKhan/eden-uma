// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <thread>
#include "common/logging.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel {

namespace {

class ThreadQueueImplForKLightLock final : public KThreadQueue {
public:
    explicit ThreadQueueImplForKLightLock(KernelCore& kernel) : KThreadQueue(kernel) {}

    void CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task) override {
        // Remove the thread as a waiter from its owner.
        if (KThread* owner = waiting_thread->GetLockOwner(kernel); owner != nullptr) {
            owner->RemoveWaiter(kernel, waiting_thread);
        }

        // Invoke the base cancel wait handler.
        KThreadQueue::CancelWait(kernel, waiting_thread, wait_result, cancel_timer_task);
    }
};

} // namespace

void KLightLock::Lock() {
    const uintptr_t cur_thread = uintptr_t(GetCurrentThreadPointer(m_kernel));
    while (true) {
        uintptr_t old_tag = m_tag.load(std::memory_order_relaxed);
        while (!m_tag.compare_exchange_weak(old_tag, (old_tag == 0) ? cur_thread : (old_tag | 1), std::memory_order_acquire))
            ;
        if (old_tag == 0 || this->LockSlowPath(old_tag | 1, cur_thread)) {
            break;
        }
    }
}

void KLightLock::Unlock() {
    const uintptr_t cur_thread = reinterpret_cast<uintptr_t>(GetCurrentThreadPointer(m_kernel));

    uintptr_t expected = cur_thread;
    if (!m_tag.compare_exchange_strong(expected, 0, std::memory_order_release)) {
        this->UnlockSlowPath(cur_thread);
    }
}

bool KLightLock::LockSlowPath(uintptr_t _owner, uintptr_t _cur_thread) {
    KThread* cur_thread = reinterpret_cast<KThread*>(_cur_thread);
    ThreadQueueImplForKLightLock wait_queue(m_kernel);

    // Pend the current thread waiting on the owner thread.
    {
        KScopedSchedulerLock sl{m_kernel};

        // Ensure we actually have locking to do.
        if (m_tag.load(std::memory_order_relaxed) != _owner) {
            return false;
        }

        // Add the current thread as a waiter on the owner.
        KThread* owner_thread = reinterpret_cast<KThread*>(_owner & ~1ULL);
        cur_thread->SetKernelAddressKey(reinterpret_cast<uintptr_t>(std::addressof(m_tag)));
        owner_thread->AddWaiter(m_kernel, cur_thread);

        // Begin waiting to hold the lock.
        cur_thread->SetLightLockWaitQueue(std::addressof(wait_queue));
        cur_thread->BeginWait(m_kernel, std::addressof(wait_queue));

        if (owner_thread->IsSuspended()) {
            owner_thread->ContinueIfHasKernelWaiters(m_kernel);
        }
    }

    // The wait queue lives in this frame. A thread that is still waiting here did not block
    // (it holds the scheduler lock, so it cannot): that is a bug in the caller.
    if (cur_thread->GetState() == ThreadState::Waiting) {
        static std::atomic<u32> early_wakes{0};
        if (early_wakes.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_CRITICAL(Kernel, "KLightLock: waited without blocking (thread {} dummy={})",
                         cur_thread->GetThreadId(), cur_thread->IsDummyThread());
        }
    }

    {
        KScopedSchedulerLock sl{m_kernel};
        cur_thread->SetLightLockWaitQueue(nullptr);
    }

    // Only an unlock passing the lock to us ends this wait with success. A termination request
    // cancels the wait instead (ResultTerminationRequested) without handing over the lock;
    // treating that as acquired ran the critical section beside the real owner and, on unlock,
    // cleared the owner's tag, so a later unlock handed the lock to a thread no longer waiting
    // for it (a crash in KThread::EndWait while a terminating applet's threads were finalized).
    // Retry instead: termination cancels only the first wait, so the retry waits normally.
    if (cur_thread->GetState() != ThreadState::Waiting &&
        cur_thread->GetWaitResult() == ResultTerminationRequested) {
        return false;
    }
    return true;
}

void KLightLock::UnlockSlowPath(uintptr_t _cur_thread) {
    KThread* owner_thread = reinterpret_cast<KThread*>(_cur_thread);

    // Unlock.
    {
        KScopedSchedulerLock sl(m_kernel);

        // Get the next owner.
        bool has_waiters;
        KThread* next_owner = owner_thread->RemoveKernelWaiterByKey(m_kernel,
            std::addressof(has_waiters), uintptr_t(std::addressof(m_tag)));

        // Pass the lock to the next owner.
        uintptr_t next_tag = 0;
        if (next_owner != nullptr) {
            next_tag = uintptr_t(next_owner) | uintptr_t(has_waiters);

            // Only end the wait of a thread blocked in this kind of wait: ending a wait through a
            // queue the thread is no longer blocked on jumps through a dead stack object. A
            // waiter left behind that way already treats itself as the owner.
            if (next_owner->GetState() == ThreadState::Waiting &&
                next_owner->GetWaitQueue() != nullptr &&
                next_owner->GetWaitQueue() == next_owner->GetLightLockWaitQueue()) {
                next_owner->EndWait(m_kernel, ResultSuccess);
            } else {
                static std::atomic<u32> stale_waiters{0};
                if (stale_waiters.fetch_add(1, std::memory_order_relaxed) < 16) {
                    LOG_CRITICAL(Kernel,
                                 "KLightLock: next owner {} is not blocked on the lock (state {}, "
                                 "dummy {}, termination requested {}, has waiters {})",
                                 next_owner->GetThreadId(),
                                 static_cast<u32>(next_owner->GetState()),
                                 next_owner->IsDummyThread(),
                                 next_owner->IsTerminationRequested(), has_waiters);
                }
            }

            if (next_owner->IsSuspended()) {
                next_owner->ContinueIfHasKernelWaiters(m_kernel);
            }
        }

        // We may have unsuspended in the process of acquiring the lock, so we'll re-suspend now if
        // so.
        if (owner_thread->IsSuspended()) {
            owner_thread->TrySuspend(m_kernel);
        }

        // Write the new tag value.
        m_tag.store(next_tag, std::memory_order_release);
    }
}

bool KLightLock::IsLockedByCurrentThread() const {
    return (m_tag.load() | 1ULL) == (uintptr_t(GetCurrentThreadPointer(m_kernel)) | 1ULL);
}

} // namespace Kernel
