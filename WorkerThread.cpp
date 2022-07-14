/*
 * Copyright (C) 2008 Apple Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "config.h"

#include "core/workers/WorkerThread.h"

#include "bindings/core/v8/ScriptSourceCode.h"
#include "bindings/core/v8/V8GCController.h"
#include "bindings/core/v8/V8Initializer.h"
#include "core/dom/Microtask.h"
#include "core/inspector/InspectorInstrumentation.h"
#include "core/inspector/WorkerInspectorController.h"
#include "core/workers/DedicatedWorkerGlobalScope.h"
#include "core/workers/WorkerClients.h"
#include "core/workers/WorkerReportingProxy.h"
#include "core/workers/WorkerThreadStartupData.h"
#include "platform/PlatformThreadData.h"
#include "platform/Task.h"
#include "platform/ThreadSafeFunctional.h"
#include "platform/ThreadTimers.h"
#include "platform/heap/SafePoint.h"
#include "platform/heap/ThreadState.h"
#include "platform/weborigin/KURL.h"
#include "public/platform/Platform.h"
#include "public/platform/WebScheduler.h"
#include "public/platform/WebThread.h"
#include "public/platform/WebWaitableEvent.h"
#include "wtf/Noncopyable.h"
#include "wtf/WeakPtr.h"
#include "wtf/text/WTFString.h"

namespace blink {

namespace {
const double kLongIdlePeriodSecs = 1.0;

} // namespace

class WorkerMicrotaskRunner : public WebThread::TaskObserver {
public:
    explicit WorkerMicrotaskRunner(WorkerThread* workerThread)
        : m_workerThread(workerThread)
    {
    }

    virtual void willProcessTask() override
    {
        // No tasks should get executed after we have closed.
        WorkerGlobalScope* globalScope = m_workerThread->workerGlobalScope();
        ASSERT_UNUSED(globalScope, !globalScope || !globalScope->isClosing());
    }

    virtual void didProcessTask() override
    {
        Microtask::performCheckpoint(m_workerThread->isolate());
        if (WorkerGlobalScope* globalScope = m_workerThread->workerGlobalScope()) {
            if (WorkerScriptController* scriptController = globalScope->script())
                scriptController->rejectedPromises()->processQueue();
            if (globalScope->isClosing()) {
                m_workerThread->workerReportingProxy().workerGlobalScopeClosed();
                m_workerThread->shutdown();
            }
        }
    }

private:
    // Thread owns the microtask runner; reference remains
    // valid for the lifetime of this object.
    WorkerThread* m_workerThread;
};

static Mutex& threadSetMutex()
{
    AtomicallyInitializedStaticReference(Mutex, mutex, new Mutex);
    return mutex;
}

static HashSet<WorkerThread*>& workerThreads()
{
    DEFINE_STATIC_LOCAL(HashSet<WorkerThread*>, threads, ());
    return threads;
}

unsigned WorkerThread::workerThreadCount()
{
    MutexLocker lock(threadSetMutex());
    return workerThreads().size();
}

class WorkerThreadCancelableTask final : public ExecutionContextTask {
    WTF_MAKE_NONCOPYABLE(WorkerThreadCancelableTask); WTF_MAKE_FAST_ALLOCATED(WorkerThreadCancelableTask);
public:
    static PassOwnPtr<WorkerThreadCancelableTask> create(PassOwnPtr<Closure> closure)
    {
        return adoptPtr(new WorkerThreadCancelableTask(closure));
    }

    virtual ~WorkerThreadCancelableTask() { }

    virtual void performTask(ExecutionContext*) override
    {
        if (!m_taskCanceled)
            (*m_closure)();
    }

    WeakPtr<WorkerThreadCancelableTask> createWeakPtr() { return m_weakFactory.createWeakPtr(); }
    void cancelTask() { m_taskCanceled = true; }

private:
    explicit WorkerThreadCancelableTask(PassOwnPtr<Closure> closure)
        : m_closure(closure)
        , m_weakFactory(this)
        , m_taskCanceled(false)
    { }

    OwnPtr<Closure> m_closure;
    WeakPtrFactory<WorkerThreadCancelableTask> m_weakFactory;
    bool m_taskCanceled;
};

class WorkerSharedTimer : public SharedTimer {
public:
    explicit WorkerSharedTimer(WorkerThread* workerThread)
        : m_workerThread(workerThread)
        , m_running(false)
    { }

    typedef void (*SharedTimerFunction)();
    virtual void setFiredFunction(SharedTimerFunction func)
    {
        m_sharedTimerFunction = func;
    }

    virtual void setFireInterval(double interval)
    {
        ASSERT(m_sharedTimerFunction);

        // See BlinkPlatformImpl::setSharedTimerFireInterval for explanation of
        // why ceil is used in the interval calculation.
        int64_t delay = static_cast<int64_t>(ceil(interval * 1000));

        if (delay < 0) {
            delay = 0;
        }

        m_running = true;

        if (m_lastQueuedTask.get())
            m_lastQueuedTask->cancelTask();

        // Now queue the task as a cancellable one.
        OwnPtr<WorkerThreadCancelableTask> task = WorkerThreadCancelableTask::create(bind(&WorkerSharedTimer::OnTimeout, this));
        m_lastQueuedTask = task->createWeakPtr();
        m_workerThread->postDelayedTask(FROM_HERE, task.release(), delay);
    }

    virtual void stop()
    {
        m_running = false;
        m_lastQueuedTask = nullptr;
    }

private:
    void OnTimeout()
    {
        ASSERT(m_workerThread->workerGlobalScope());

        m_lastQueuedTask = nullptr;

        if (m_sharedTimerFunction && m_running && !m_workerThread->workerGlobalScope()->isClosing())
            m_sharedTimerFunction();
    }

    WorkerThread* m_workerThread;
    SharedTimerFunction m_sharedTimerFunction;
    bool m_running;

    // The task to run OnTimeout, if any. While OnTimeout resets
    // m_lastQueuedTask, this must be a weak pointer because the
    // worker runloop may delete the task as it is shutting down.
    WeakPtr<WorkerThreadCancelableTask> m_lastQueuedTask;
};

class WorkerThreadTask : public WebThread::Task {
    WTF_MAKE_NONCOPYABLE(WorkerThreadTask); WTF_MAKE_FAST_ALLOCATED(WorkerThreadTask);
public:
    static PassOwnPtr<WorkerThreadTask> create(WorkerThread& workerThread, PassOwnPtr<ExecutionContextTask> task, bool isInstrumented)
    {
        return adoptPtr(new WorkerThreadTask(workerThread, task, isInstrumented));
    }

    virtual ~WorkerThreadTask() { }

    virtual void run() override
    {
        WorkerGlobalScope* workerGlobalScope = m_workerThread.workerGlobalScope();
        // If the thread is terminated before it had a chance initialize (see
        // WorkerThread::Initialize()), we mustn't run any of the posted tasks.
        if (!workerGlobalScope) {
            ASSERT(m_workerThread.terminated());
            return;
        }

        if (m_isInstrumented)
            InspectorInstrumentation::willPerformExecutionContextTask(workerGlobalScope, m_task.get());
        m_task->performTask(workerGlobalScope);
        if (m_isInstrumented)
            InspectorInstrumentation::didPerformExecutionContextTask(workerGlobalScope);
    }

private:
    WorkerThreadTask(WorkerThread& workerThread, PassOwnPtr<ExecutionContextTask> task, bool isInstrumented)
        : m_workerThread(workerThread)
        , m_task(task)
        , m_isInstrumented(isInstrumented)
    {
        if (m_isInstrumented)
            m_isInstrumented = !m_task->taskNameForInstrumentation().isEmpty();
        if (m_isInstrumented)
            InspectorInstrumentation::didPostExecutionContextTask(m_workerThread.workerGlobalScope(), m_task.get());
    }

    WorkerThread& m_workerThread;
    OwnPtr<ExecutionContextTask> m_task;
    bool m_isInstrumented;
};

WorkerThread::WorkerThread(PassRefPtr<WorkerLoaderProxy> workerLoaderProxy, WorkerReportingProxy& workerReportingProxy)
    : m_started(false)
    , m_terminated(false)
    , m_shutdown(false)
    , m_workerLoaderProxy(workerLoaderProxy)
    , m_workerReportingProxy(workerReportingProxy)
    , m_webScheduler(nullptr)
    , m_isolate(nullptr)
    , m_shutdownEvent(adoptPtr(Platform::current()->createWaitableEvent()))
    , m_terminationEvent(adoptPtr(Platform::current()->createWaitableEvent()))
{
    MutexLocker lock(threadSetMutex());
    workerThreads().add(this);
}

WorkerThread::~WorkerThread()
{
    MutexLocker lock(threadSetMutex());
    ASSERT(workerThreads().contains(this));
    workerThreads().remove(this);
}

void WorkerThread::start(PassOwnPtr<WorkerThreadStartupData> startupData)
{
    if (m_started)
        return;

    m_started = true;
    backingThread().postTask(FROM_HERE, new Task(threadSafeBind(&WorkerThread::initialize, AllowCrossThreadAccess(this), startupData)));
}

void WorkerThread::interruptAndDispatchInspectorCommands()
{
    MutexLocker locker(m_workerInspectorControllerMutex);
    if (m_workerInspectorController)
        m_workerInspectorController->interruptAndDispatchInspectorCommands();
}

PlatformThreadId WorkerThread::platformThreadId()
{
    if (!m_started)
        return 0;
    return backingThread().platformThread().threadId();
}

void WorkerThread::initialize(PassOwnPtr<WorkerThreadStartupData> startupData)
{
    KURL scriptURL = startupData->m_scriptURL;
    String sourceCode = startupData->m_sourceCode;
    WorkerThreadStartMode startMode = startupData->m_startMode;
    OwnPtr<Vector<char>> cachedMetaData = startupData->m_cachedMetaData.release();
    V8CacheOptions v8CacheOptions = startupData->m_v8CacheOptions;

    m_webScheduler = backingThread().platformThread().scheduler();
    {
        MutexLocker lock(m_threadStateMutex);

        // The worker was terminated before the thread had a chance to run.
        if (m_terminated) {
            // Notify the proxy that the WorkerGlobalScope has been disposed of.
            // This can free this thread object, hence it must not be touched afterwards.
            m_workerReportingProxy.workerThreadTerminated();
            return;
        }

        m_microtaskRunner = adoptPtr(new WorkerMicrotaskRunner(this));
        backingThread().addTaskObserver(m_microtaskRunner.get());
        backingThread().initialize();

        m_isolate = initializeIsolate();
        m_workerGlobalScope = createWorkerGlobalScope(startupData);
        m_workerGlobalScope->scriptLoaded(sourceCode.length(), cachedMetaData.get() ? cachedMetaData->size() : 0);

        PlatformThreadData::current().threadTimers().setSharedTimer(adoptPtr(new WorkerSharedTimer(this)));
    }

    // The corresponding call to stopRunLoop() is in ~WorkerScriptController().
    didStartRunLoop();

    // Notify proxy that a new WorkerGlobalScope has been created and started.
    m_workerReportingProxy.workerGlobalScopeStarted(m_workerGlobalScope.get());

    WorkerScriptController* script = m_workerGlobalScope->script();
    if (!script->isExecutionForbidden())
        script->initializeContextIfNeeded();
    if (startMode == PauseWorkerGlobalScopeOnStart)
        m_workerGlobalScope->workerInspectorController()->pauseOnStart();

    OwnPtr<CachedMetadataHandler> handler(workerGlobalScope()->createWorkerScriptCachedMetadataHandler(scriptURL, cachedMetaData.get()));
    bool success = script->evaluate(ScriptSourceCode(sourceCode, scriptURL), nullptr, handler.get(), v8CacheOptions);
    m_workerGlobalScope->didEvaluateWorkerScript();
    m_workerReportingProxy.didEvaluateWorkerScript(success);

    postInitialize();

    m_webScheduler->postIdleTaskAfterWakeup(FROM_HERE, WTF::bind<double>(&WorkerThread::performIdleWork, this));
}

void WorkerThread::shutdown()
{
    ASSERT(isCurrentThread());
    {
        MutexLocker lock(m_threadStateMutex);
        ASSERT(!m_shutdown);
        m_shutdown = true;
    }

    PlatformThreadData::current().threadTimers().setSharedTimer(nullptr);
    workerGlobalScope()->dispose();
    willDestroyIsolate();

    // This should be called before we start the shutdown procedure.
    workerReportingProxy().willDestroyWorkerGlobalScope();

    // The below assignment will destroy the context, which will in turn notify messaging proxy.
    // We cannot let any objects survive past thread exit, because no other thread will run GC or otherwise destroy them.
    // If Oilpan is enabled, we detach of the context/global scope, with the final heap cleanup below sweeping it out.
#if !ENABLE(OILPAN)
    ASSERT(m_workerGlobalScope->hasOneRef());
#endif
    m_workerGlobalScope->notifyContextDestroyed();
    m_workerGlobalScope = nullptr;

    backingThread().removeTaskObserver(m_microtaskRunner.get());
    backingThread().shutdown();
    destroyIsolate();

    m_microtaskRunner = nullptr;

    // Notify the proxy that the WorkerGlobalScope has been disposed of.
    // This can free this thread object, hence it must not be touched afterwards.
    workerReportingProxy().workerThreadTerminated();

    m_terminationEvent->signal();

    // Clean up PlatformThreadData before WTF::WTFThreadData goes away!
    PlatformThreadData::current().destroy();
}


void WorkerThread::stop()
{
    // Prevent the deadlock between GC and an attempt to stop a thread.
    SafePointScope safePointScope(ThreadState::HeapPointersOnStack);
    stopInternal();
}

void WorkerThread::stopInShutdownSequence()
{
    stopInternal();
}

void WorkerThread::terminateAndWait()
{
    stop();
    m_terminationEvent->wait();
}

bool WorkerThread::terminated()
{
    MutexLocker lock(m_threadStateMutex);
    return m_terminated;
}

void WorkerThread::stopInternal()
{
    // Protect against this method, initialize() or termination via the global scope racing each other.
    MutexLocker lock(m_threadStateMutex);

    // If stop has already been called, just return.
    if (m_terminated)
        return;
    m_terminated = true;

    // Signal the thread to notify that the thread's stopping.
    if (m_shutdownEvent)
        m_shutdownEvent->signal();

    // If the thread has already initiated shut down, just return.
    if (m_shutdown)
        return;

    // If the worker thread was never initialized, complete the termination immediately.
    if (!m_workerGlobalScope) {
        m_terminationEvent->signal();
        return;
    }

    // Ensure that tasks are being handled by thread event loop. If script execution weren't forbidden, a while(1) loop in JS could keep the thread alive forever.
    terminateV8Execution();

    InspectorInstrumentation::didKillAllExecutionContextTasks(m_workerGlobalScope.get());
    m_debuggerMessageQueue.kill();
    backingThread().postTask(FROM_HERE, new Task(threadSafeBind(&WorkerThread::shutdown, AllowCrossThreadAccess(this))));
}

void WorkerThread::didStartRunLoop()
{
    ASSERT(isCurrentThread());
    Platform::current()->didStartWorkerRunLoop();
}

void WorkerThread::didStopRunLoop()
{
    ASSERT(isCurrentThread());
    Platform::current()->didStopWorkerRunLoop();
}

void WorkerThread::terminateAndWaitForAllWorkers()
{
    // Keep this lock to prevent WorkerThread instances from being destroyed.
    MutexLocker lock(threadSetMutex());
    HashSet<WorkerThread*> threads = workerThreads();
    for (WorkerThread* thread : threads)
        thread->stopInShutdownSequence();

    for (WorkerThread* thread : threads)
        thread->terminationEvent()->wait();
}

bool WorkerThread::isCurrentThread()
{
    return m_started && backingThread().isCurrentThread();
}

void WorkerThread::performIdleWork(double deadlineSeconds)
{
    double gcDeadlineSeconds = deadlineSeconds;

    // The V8 GC does some GC steps (e.g. compaction) only when the idle notification is ~1s.
    // TODO(rmcilroy): Refactor so extending the deadline like this this isn't needed.
    if (m_webScheduler->canExceedIdleDeadlineIfRequired())
        gcDeadlineSeconds = Platform::current()->monotonicallyIncreasingTime() + kLongIdlePeriodSecs;

    if (doIdleGc(gcDeadlineSeconds))
        m_webScheduler->postIdleTaskAfterWakeup(FROM_HERE, WTF::bind<double>(&WorkerThread::performIdleWork, this));
    else
        m_webScheduler->postIdleTask(FROM_HERE, WTF::bind<double>(&WorkerThread::performIdleWork, this));
}

bool WorkerThread::doIdleGc(double deadlineSeconds)
{
    bool gcFinished = false;
    if (deadlineSeconds > Platform::current()->monotonicallyIncreasingTime())
        gcFinished = isolate()->IdleNotificationDeadline(deadlineSeconds);
    return gcFinished;
}

void WorkerThread::postTask(const WebTraceLocation& location, PassOwnPtr<ExecutionContextTask> task)
{
    backingThread().postTask(location, WorkerThreadTask::create(*this, task, true).leakPtr());
}

void WorkerThread::postDelayedTask(const WebTraceLocation& location, PassOwnPtr<ExecutionContextTask> task, long long delayMs)
{
    backingThread().postDelayedTask(location, WorkerThreadTask::create(*this, task, true).leakPtr(), delayMs);
}

v8::Isolate* WorkerThread::initializeIsolate()
{
    ASSERT(isCurrentThread());
    ASSERT(!m_isolate);
    v8::Isolate* isolate = V8PerIsolateData::initialize();
    V8Initializer::initializeWorker(isolate);

    m_interruptor = adoptPtr(new V8IsolateInterruptor(isolate));
    ThreadState::current()->addInterruptor(m_interruptor.get());
    ThreadState::current()->registerTraceDOMWrappers(isolate, V8GCController::traceDOMWrappers);

    return isolate;
}

void WorkerThread::willDestroyIsolate()
{
    ASSERT(isCurrentThread());
    ASSERT(m_isolate);
    V8PerIsolateData::willBeDestroyed(m_isolate);
    ThreadState::current()->removeInterruptor(m_interruptor.get());
}

void WorkerThread::destroyIsolate()
{
    ASSERT(isCurrentThread());
    V8PerIsolateData::destroy(m_isolate);
    m_isolate = nullptr;
}

void WorkerThread::terminateV8Execution()
{
    ASSERT(isMainThread());
    m_workerGlobalScope->script()->willScheduleExecutionTermination();
    v8::V8::TerminateExecution(m_isolate);
}

void WorkerThread::appendDebuggerTask(PassOwnPtr<WebThread::Task> task)
{
    m_debuggerMessageQueue.append(task);
}

MessageQueueWaitResult WorkerThread::runDebuggerTask(WaitMode waitMode)
{
    ASSERT(isCurrentThread());
    MessageQueueWaitResult result;
    double absoluteTime = MessageQueue<WebThread::Task>::infiniteTime();
    OwnPtr<WebThread::Task> task;
    {
        if (waitMode == DontWaitForMessage)
            absoluteTime = 0.0;
        SafePointScope safePointScope(ThreadState::NoHeapPointersOnStack);
        task = m_debuggerMessageQueue.waitForMessageWithTimeout(result, absoluteTime);
    }

    if (result == MessageQueueMessageReceived) {
        InspectorInstrumentation::willProcessTask(workerGlobalScope());
        task->run();
        InspectorInstrumentation::didProcessTask(workerGlobalScope());
    }

    return result;
}

void WorkerThread::willEnterNestedLoop()
{
    InspectorInstrumentation::willEnterNestedRunLoop(m_workerGlobalScope.get());
}

void WorkerThread::didLeaveNestedLoop()
{
    InspectorInstrumentation::didLeaveNestedRunLoop(m_workerGlobalScope.get());
}

void WorkerThread::setWorkerInspectorController(WorkerInspectorController* workerInspectorController)
{
    MutexLocker locker(m_workerInspectorControllerMutex);
    m_workerInspectorController = workerInspectorController;
}

} // namespace blink
