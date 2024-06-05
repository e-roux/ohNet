#ifndef HEADER_THREAD
#define HEADER_THREAD

#include <OpenHome/Private/Standard.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Functor.h>
#include <OpenHome/OsTypes.h>

#include <vector>
#include <memory>

EXCEPTION(ThreadKill)
EXCEPTION(Timeout)

namespace OpenHome {

enum ThreadPriority {
    kPriorityMuchMore = 4
   ,kPriorityMore = 2
   ,kPriorityLess = -2
   ,kPriorityMuchLess = -4

   ,kPrioritySystemLowest = 1
   ,kPriorityLowest = 10
   ,kPriorityVeryLow = 20
   ,kPriorityLower = 30
   ,kPriorityLow = 40
   ,kPriorityNormal = 50
   ,kPriorityHigh = 60
   ,kPriorityHigher = 70
   ,kPriorityVeryHigh = 80
   ,kPriorityHighest = 90
   ,kPrioritySystemHighest = 100
};

class Semaphore : public INonCopyable
{
public:
    static const TUint kWaitForever = 0;
    Semaphore(const TChar* aName, TUint aCount);
    virtual ~Semaphore();

    void Wait();
    // Note: aTimeoutMs == 0 means wait forever
    void Wait(TUint aTimeoutMs);
    /**
     * Clear any pending signals
     *
     * @return  true if signals were cleared; false otherwise
     */
    TBool Clear();
    void Signal();
private:
    THandle iHandle;
};

class DllExportClass IMutex
{
public:
    DllExport virtual ~IMutex() {};

    DllExport virtual void Wait() = 0;
    DllExport virtual void Signal() = 0;
    DllExport virtual const TChar* Name() const = 0;
};

class DllExportClass Mutex : public INonCopyable
{
private:
    static const TChar* kErrorStringDeadlock;
    static const TChar* kErrorStringUninitialised;
public:
    DllExport Mutex(const TChar* aName);
    DllExport virtual ~Mutex();

    DllExport void Wait();
    DllExport void Signal();
protected:
    THandle iHandle;
    TChar iName[5];
};

class DllExportClass MutexFactory
{
public:
    DllExport static IMutex* Create(const TChar* aName, TBool aInstrumented = false, TUint64 aInstrumentedTriggerUs = 0);

private:
    MutexFactory() {};
};

/**
 * Abstract runnable thread class
 *
 * Only threads created using this library can be treated as Thread objects
 */
class Thread : public INonCopyable
{
    friend class SemaphoreActive;
public:
    static const TUint kDefaultStackBytes;
    static const TUint kMaxNameBytes = 18;
public:
    /**
     * Destructor.  Automatically calls Join()
     */
    virtual ~Thread();

    /**
     * Second phase constructor.  The thread will not Run() until this is called
     */
    void Start();
public:
    /**
     * Wait on the thread being signalled or killed.
     * Will not automatically complete when the thread exits.
     * Throws ThreadKill if Kill() has been called.
     */
    void Wait();

    /**
     * Completes one (current or future) caller to Wait()
     */
    void Signal();

    /**
     * Wait()s only if the thread has already been signalled.
     * Throws ThreadKill if Kill() has been called.
     */
    TBool TryWait();

    /**
     * Wait on the per-thread high-performance semaphore.
     * Does _not_ check for kill.
     * May only be called on the Thread object associated with the current thread.
     */
    void NotifyWait();

    /**
     * Wait on the per-thread high-performance semaphore.
     * Clears semaphore before returning.
     * Does _not_ check for kill.
     * May only be called on the Thread object associated with the current thread.
     */
    void NotifyWaitAll();

    /**
     * Completes one (current or future) caller to NotifyWait() or NotifyWaitAll().
     */
    void NotifySignal();

    /**
     * Pause the current thread.  Won't automatically block other threads but may
     * block some if this thread holds resources they are waiting for.
     *
     * @param aMilliSecs   Number of milliseconds to sleep for
     */
    static void Sleep(TUint aMilliSecs);
    
    /**
     * Return the name of the current thread.
     * Or "____" for threads non-OpenHome threads.
     */
    static const Brx& CurrentThreadName();

    /**
     * Return a pointer to the current thread, or NULL if running in a non-OpenHome thread.
     */
    static Thread* Current();

    /**
     * Query whether the platform port supports thread priorities
     */
    static TBool SupportsPriorities();

    /**
     * If current thread is an OpenHome thread, and Kill() has been called, then
     * throw ThreadKill.
     */
    static void CheckCurrentForKill();

    /**
     * Mark a thread as to be killed.  The thread will not exit immediately
     */
    void Kill();

    /**
     * Returns the thread's 4 char name
     */
    const Brx& Name() const;

    /**
     * Intended for use from derived destructors.  Multiple levels of dtor can all Join() safely
     */
    void Join();

    bool operator== (const Thread&) const;
    bool operator!= (const Thread&) const;
protected:
    /**
     * Constructor.
     *
     * @param aName        Thread name.  Max 4 chars; need not be unique
     * @param aPriority    Priority to run the thread at
     * @param aStackBytes  Stack size in bytes
     */
    Thread(const TChar* aName, TUint aPriority = kPriorityNormal, TUint aStackBytes = kDefaultStackBytes);

    /**
     * Derived classes should override the Run() method. Default implementation does nothing.
     */
    virtual void Run();

    /**
     * Throws ThreadKill if Kill() has been called.
     */
    void CheckForKill() const;
private:
    static void EntryPoint(void* aArg);
protected:
    THandle iHandle;
    Bws<kMaxNameBytes+1> iName;
    Semaphore iSema;
private:
    Semaphore iProceedSema;
    Semaphore iRunningSema;
    Semaphore iTerminated;
    TBool     iKill;
    mutable Mutex iKillMutex;
};

class DllExportClass MutexInstrumented : public Mutex
{
public:
    DllExport MutexInstrumented(const TChar* aName, TUint64 aWaitTriggerUs);
    DllExport virtual ~MutexInstrumented() {};

    DllExport void Wait();
    DllExport void Signal();
private:
    TUint64 iWaitTriggerUs;
    Bws<Thread::kMaxNameBytes+1> iLastUseThreadName;
};


/**
 * Create a custom thread without needing to create a custom subclass
 */
class ThreadFunctor : public Thread
{
public:
    /**
     * Constructor.
     *
     * @param aName        Thread name.  Max 4 chars; need not be unique
     * @param aFunctor     Entrypoint function to run
     * @param aPriority    Priority to run the thread at
     * @param aStackBytes  Stack size in bytes
     */
    ThreadFunctor(const TChar* aName, Functor aFunctor, TUint aPriority = kPriorityNormal, TUint aStackBytes = kDefaultStackBytes);
    ~ThreadFunctor();
private:
    virtual void Run();
private:
    Functor iFunctor;
};

class IPriorityArbitrator
{
public:
    virtual ~IPriorityArbitrator() {}
    virtual TUint Priority(const TChar* aId, TUint aRequested, TUint aHostMax) = 0;
    virtual TUint OpenHomeMin() const = 0;
    virtual TUint OpenHomeMax() const = 0;
    virtual TUint HostRange() const = 0;
};

class ThreadPriorityArbitrator
{
    friend class Environment;
public:
    ThreadPriorityArbitrator(TUint aHostMin, TUint aHostMax);
    void Add(IPriorityArbitrator& aArbitrator);
    void Validate();
    TUint CalculatePriority(const char* aId, TUint aRequested) const;
    static TUint DoCalculatePriority(TUint aRequested, TUint aOpenHomeMin, TUint aOpenHomeMax, TUint aHostMin, TUint aHostMax);
private:
    std::vector<IPriorityArbitrator*> iArbitrators;
    TUint iHostMin;
    TUint iHostMax;
};

/**
 * Utility class.
 *
 * Create an AutoMutex on the stack using a reference to a Mutex. It will
 * automatically be signalled on stack cleanup (ie on return or when an
 * exception passes up).
 */
class DllExportClass AutoMutex : public INonCopyable
{
public:
    DllExport AutoMutex(Mutex& aMutex);
    DllExport ~AutoMutex();
private:
    Mutex& iMutex;
};

/**
 * Utility class.
 *
 * Create an AutoSemaphore on the stack using a reference to a Semaphore.
 * Acquire (Wait() for) the semaphore on construction, release (Signal())
 * automatically on stack cleanup (ie on return or when an exception passes up).
 */
class DllExportClass AutoSemaphore : public INonCopyable
{
public:
    DllExport AutoSemaphore(Semaphore& aSemaphore);
    DllExport ~AutoSemaphore();
private:
    Semaphore& iSem;
};

/**
 * Utility class.
 *
 * Create an AutoSemaphore on the stack using a reference to a Semaphore.
 * Does not acquire (Wait() for) the semaphore on construction, releases (Signal())
 * automatically on stack cleanup (ie on return or when an exception passes up).
 */
class DllExportClass AutoSemaphoreSignal : public INonCopyable
{
public:
    DllExport AutoSemaphoreSignal(Semaphore& aSemaphore);
    DllExport ~AutoSemaphoreSignal();
private:
    Semaphore& iSem;
};

} // namespace OpenHome

#endif
