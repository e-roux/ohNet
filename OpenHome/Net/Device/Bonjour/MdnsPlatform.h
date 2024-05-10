#ifndef HEADER_MDNSPLATFORM
#define HEADER_MDNSPLATFORM

#include <OpenHome/Private/Standard.h>
#include <OpenHome/Private/Fifo.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Timer.h>
#include <OpenHome/Private/Network.h>
#include <OpenHome/Net/Core/OhNet.h>
#include <OpenHome/Net/Private/FunctorCpiDevice.h>

// Bonjour source code available from  http://developer.apple.com/networking/bonjour/index.html
// We take the mDNSCore folder from their source release and discard the rest

#include <OpenHome/Net/Private/mDNSEmbeddedAPI.h>
#include <OpenHome/Net/Private/dns_sd.h>

#include <vector>
#include <map>

EXCEPTION(MdnsImpossibleEvent);
EXCEPTION(MdnsDuplicateEvent);

namespace OpenHome {

class Environment;

namespace Net {

class MdnsDevice{
public:
    MdnsDevice(const Brx& aType, const Brx& aFriendlyName, const Brx& aUglyName, const Brx& aIpAddress, TUint aPort);
    virtual ~MdnsDevice() {};

    const Brx& Type();
    const Brx& FriendlyName();
    const Brx& UglyName();
    const Brx& IpAddress();
    const TUint Port();
private:
    Brn iType;
    Brn iFriendlyName;
    Brn iUglyName;
    Brn iIpAddress;
    TUint iPort;
};

class IMdnsDeviceListener
{
public:
    virtual void DeviceAdded(MdnsDevice& aDev) = 0;
    virtual ~IMdnsDeviceListener() {}
};

/*
 * Implementation must be thread-safe, as there may be multiple callers on different threads.
 */
class IMdnsMulticastPacketReceiver
{
public:
    virtual ~IMdnsMulticastPacketReceiver() {}
    virtual void ReceiveMulticastPacket(const Brx& aMsg, const Endpoint aSrc, const Endpoint aDst) = 0;
};

class MulticastListener : private INonCopyable
{
private:
    static const TUint kMaxMessageBytes = 4096;
    static const Brn kAddressV4;
    static const Brn kAddressV6;
    static const TUint kListenPort = 5353;
public:
    MulticastListener(Environment& aEnv, IMdnsMulticastPacketReceiver& aReceiver);
    ~MulticastListener();
    void Start();
    void Stop();
    void Clear();
    /*
     * THROWS NetworkError if failure to bind to aAddress.
     *
     * For anything other than the first call to Bind(), Clear() must have been called first.
     */
    void Bind(const TIpAddress& aAddress);
private:
    void ThreadListen();
private:
    class ReadWriteLock
    {
    public:
        ReadWriteLock();
    public:
        void AcquireReadLock();
        void ReleaseReadLock();
        void AcquireWriteLock();
        void ReleaseWriteLock();
    private:
        TUint iReaderCount;
        Mutex iLockReaders;
        Mutex iLockWriter;
    };
private:
    Endpoint iMulticast;
    Environment& iEnv;
    IMdnsMulticastPacketReceiver& iReceiver;

    // iReader and iReaderController must be protected by iMulticastLock.
    SocketUdpMulticast* iReader;
    UdpReader* iReaderController;
    ReadWriteLock iMulticastLock;
    Semaphore iSemReader;

    ThreadFunctor* iThreadListen;
    Bws<kMaxMessageBytes> iMessage;
    TBool iStop;
    Mutex iLock;
};

class MulticastListeners : private INonCopyable
{
public:
    MulticastListeners(Environment& aEnv, IMdnsMulticastPacketReceiver& aReceiver);
    ~MulticastListeners();
public:
    void Start();
    void Stop();
    /*
     * THROWS NetworkError if failure to bind to any adapter.
     *
     * This must be called on ANY subnet list change or adapter change event to
     * allow this class to determine what adapters have appeared/disappeared
     * and bind/unbind as appropriate to/from those adapters.
     */
    void Rebind(std::vector<NetworkAdapter*>& aAdapters);
private:
    void CreateListenerLocked();
    void ClearListenersLocked();
    TBool AdapterIsSuitableListener(const TIpAddress& aAdapter);
private:
    Environment& iEnv;
    IMdnsMulticastPacketReceiver& iReceiver;
    TBool iStarted;
    TBool iStopped;
    std::vector<MulticastListener*> iListeners;
    Mutex iLock;
};

class MdnsPlatform : public IMdnsMulticastPacketReceiver
{
    typedef mStatus Status;

    static const TUint kMaxHostBytes = 16;
    static const TUint kMaxQueueLength = 25;
    static const TChar* kNifCookie;
    static const TUint kInterfaceIdPoolSize = 10;
public:
    static const TUint kRRCacheSize = 32;
    static const TUint kRRCacheBytes = kRRCacheSize * sizeof(CacheRecord);
public: 
    MdnsPlatform(Environment& aStack, const TChar* aHost, TBool aHasCache);
    ~MdnsPlatform();
    Status Init();
    void Lock();
    void Unlock();
    void Close();
    Status GetPrimaryInterface(mDNSAddr* aInterfaceV4, mDNSAddr* aInterfaceV6, mDNSAddr* aRouter);
    Status SendUdp(const Brx& aBuffer, const Endpoint& aEndpoint);
    void SetHostName(const TChar* aName);
    TUint CreateService();
    void DeregisterService(TUint aHandle);
    void RegisterService(TUint aHandle, const TChar* aName, const TChar* aType, const TIpAddress& aInterface, TUint aPort, const TChar* aInfo);
    void RenameAndReregisterService(TUint aHandle, const TChar* aName);
    void AppendTxtRecord(Bwx& aBuffer, const TChar* aKey, const TChar* aValue);

    void DeviceDiscovered(const Brx& aType, const Brx& aFriendlyName, const Brx& aUglyName, const Brx&  aIpAddress, TUint aPort); // called from extern C mDNS callback DNSResolveReply
    void AddMdnsDeviceListener(IMdnsDeviceListener* aListener);
    TBool FindDevices(const TChar* aServiceName);
private: // from IMdnsMulticastPacketReceiver
    void ReceiveMulticastPacket(const Brx& aMsg, const Endpoint aSrc, const Endpoint aDst);
private:
    void ServiceThread();
    void TimerExpired();
    void SubnetListChanged();
    void CurrentAdapterChanged();
    void UpdateInterfaceList();
    Status AddValidInterfaces(std::vector<NetworkAdapter*>& aSubnetList);
    TBool InterfaceIsValid(const TIpAddress& aAdapter);
    void DoSetHostName();
    Status AddInterface(NetworkAdapter* aNif);
    TInt InterfaceIndex(const NetworkAdapter& aNif);
    TInt InterfaceIndex(const NetworkAdapter& aNif, const std::vector<NetworkAdapter*>& aList);
    static TBool NifsMatch(const NetworkAdapter& aNif1, const NetworkAdapter& aNif2);
    static void SetAddress(mDNSAddr& aAddress, const Endpoint& aEndpoint);
    static void SetPort(mDNSIPPort& aPort, const Endpoint& aEndpoint);
    static void SetPort(mDNSIPPort& aPort, TUint aValue);
    static void SetDomainLabel(domainlabel& aLabel, const TChar* aBuffer);
    static void SetDomainName(domainname& aName, const TChar* aBuffer);
    static void ServiceCallback(mDNS* aCore, ServiceRecordSet* aRecordSet, mStatus aStatus);
    static void StatusCallback(mDNS *const m, mStatus aStatus);
private:
    class Nif : INonCopyable
    {
    public:
        Nif(NetworkAdapter& aNif, NetworkInterfaceInfo* aMdnsInfo);
        ~Nif();
        NetworkAdapter& Adapter();
        NetworkInterfaceInfo& Info();
        const TIpAddress& Address() const;
        TBool ContainsAddress(const TIpAddress& aAddress) const;
    private:
        NetworkAdapter& iNif;
        NetworkInterfaceInfo* iMdnsInfo;
    };
    class InterfaceIdAllocator
    {
    public:
        InterfaceIdAllocator();
        ~InterfaceIdAllocator();

        mDNSInterfaceID AllocateId(NetworkAdapter* aInterface);
        void DeallocateId(mDNSInterfaceID aInterfaceId);
        mDNSInterfaceID GetIdForAddress(const TIpAddress& aAddress);
    private:
        FifoLite<TUint, kInterfaceIdPoolSize> iIdPool;
        std::map<TUint, NetworkAdapter*> iInterfaces;
    };
    enum MdnsServiceAction
    {
        eInvalid,
        eRegister,
        eDeregister,
        eRenameAndReregister,
    };
    class MdnsService
    {
    public:
        MdnsService(mDNS& aMdns);
        void Set(MdnsServiceAction aAction, TUint aHandle, ServiceRecordSet& aService, const TChar* aName, const TChar* aType, const mDNSInterfaceID aInterfaceId, TUint aPort, const TChar* aInfo);
        TUint PerformAction();
    private:
        TUint Register();
        TUint Deregister();
        TUint RenameAndReregister();
    private:
        // NOTE: all buffer sizes taken from mDNSEmbeddedAPI.h
        mDNS& iMdns;
        MdnsServiceAction iAction;
        TUint iHandle;
        ServiceRecordSet* iService;
        Bws<MAX_DOMAIN_LABEL-1> iName;
        Bws<MAX_DOMAIN_NAME-1> iType;
        mDNSInterfaceID iInterfaceId;
        TUint iPort;
        Bws<2048> iInfo;
    };
    class MdnsEventScheduler
    {
    // MdnsEventSchedulers public interface functions MUST be called with mDNS_Lock() held
    private:
        static const TInt kEventInvalid = 0;
        static const TUint kEventRetryMs = 50;
    public:
        MdnsEventScheduler(Environment& aStack, mDNS* aMdns);
        ~MdnsEventScheduler();
        void TrySchedule(TInt aEvent); // can throw MdnsDuplicateEvent, or MdnsImpossibleEvent
        TBool Enabled();
        void SetEnabled(TBool aEnable);
    private:
        void TimerExpired();
        void ScheduleNow();
    private:
        mDNS* iMdns;
        Timer* iTimer;
        TInt iNextEvent;
        TBool iEnabled;
        Mutex iLock;
    };
    /*
     * Simple recursive mutex.
     *
     * Only works if called from at most 1 thread not created by ohNet.
     * (i.e. one thread where Thread::Current() returns NULL).
     */
    class MutexRecursive
    {
    #define Thread_None ((Thread*)1) // assumed invalid thread address
    public:
        MutexRecursive();
        ~MutexRecursive();
        void Lock();
        void Unlock();
    private:
        Mutex iMutex;
        Thread* iOwner;
        TUint iCount;
    };
private:
    Environment& iEnv;
    Brhz iHost;
    TBool iHasCache;
    MutexRecursive iMutex;
    MdnsEventScheduler* iEventScheduler;
    MulticastListeners iListeners;
    TBool iIPv6Enabled;
    SocketUdp iClient;
    mDNS* iMdns;
    Mutex iInterfacesLock;
    std::vector<MdnsPlatform::Nif*> iInterfaces;
    InterfaceIdAllocator iInterfaceIdAllocator;
    TUint iSubnetListChangeListenerId;
    TUint iCurrentAdapterChangeListenerId;
    typedef std::map<TUint, ServiceRecordSet*> Map;
    Mutex iServicesLock;
    Fifo<MdnsService*> iFifoFree;
    Fifo<MdnsService*> iFifoPending;
    Semaphore iSem;
    ThreadFunctor* iThreadService;
    Map iServices;
    TUint iNextServiceIndex;
    TBool iStop;
    std::vector<DNSServiceRef*> iSdRefs;
    std::vector<IMdnsDeviceListener*> iDeviceListeners;
    CacheEntity* iMdnsCache;
    std::vector<void*> iDynamicCache;
    Mutex iDiscoveryLock;
    Mutex iMulticastReceiveLock;
};

} // namespace Net
} // namespace OpenHome

#endif // HEADER_MDNSPLATFORM
