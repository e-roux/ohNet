#include <OpenHome/Net/Private/MdnsPlatform.h>
#include <OpenHome/Functor.h>
#include <OpenHome/Os.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Private/Network.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Timer.h>
#include <OpenHome/Private/Arch.h>
#include <OpenHome/Private/Env.h>
#include <OpenHome/Private/NetworkAdapterList.h>
#include <OpenHome/Net/Core/OhNet.h>
#include <OpenHome/Net/Private/Globals.h>
#include <OpenHome/Private/TIpAddressUtils.h>

#include <stdlib.h>
#include <string.h>

#include <OpenHome/Net/Private/mDNSEmbeddedAPI.h>

using namespace OpenHome;
using namespace OpenHome::Net;

extern "C" {
    mDNS mDNSStorage; // required by dnssd_clientshim.c
}

// MdnsDevice
MdnsDevice::MdnsDevice(const Brx& aType, const Brx& aFriendlyName, const Brx& aUglyName, const Brx& aIpAddress, TUint aPort)
{
    iType.Set(aType);
    iFriendlyName.Set(aFriendlyName);
    iUglyName.Set(aUglyName);
    iIpAddress.Set(aIpAddress);
    iPort = aPort;
}

const Brx& MdnsDevice::Type()
{
    return iType;
}

const Brx& MdnsDevice::FriendlyName()
{
    return iFriendlyName;
}

const Brx& MdnsDevice::UglyName()
{
    return iUglyName;
}

const Brx& MdnsDevice::IpAddress()
{
    return iIpAddress;
}

const TUint MdnsDevice::Port()
{
    return iPort;
}

// MdnsPlatform

MdnsPlatform::Nif::Nif(NetworkAdapter& aNif, NetworkInterfaceInfo* aMdnsInfo)
    : iNif(aNif)
    , iMdnsInfo(aMdnsInfo)
{
    iNif.AddRef("MdnsPlatform::Nif");
}

MdnsPlatform::Nif::~Nif()
{
    iNif.RemoveRef("MdnsPlatform::Nif");
    free(iMdnsInfo);
}

NetworkAdapter& MdnsPlatform::Nif::Adapter()
{
    return iNif;
}

NetworkInterfaceInfo& MdnsPlatform::Nif::Info()
{
    return *iMdnsInfo;
}

const TIpAddress& MdnsPlatform::Nif::Address() const
{
    return iNif.Address();
}

TBool MdnsPlatform::Nif::ContainsAddress(const TIpAddress& aAddress) const
{
    return iNif.ContainsAddress(aAddress);
}

// InterfaceIdAllocator

MdnsPlatform::InterfaceIdAllocator::InterfaceIdAllocator()
{
    for (TUint i = 1; i <= iIdPool.SlotsFree(); i++) iIdPool.Write(i);
}

MdnsPlatform::InterfaceIdAllocator::~InterfaceIdAllocator()
{
    ASSERT(iInterfaces.size() == 0);
}

mDNSInterfaceID MdnsPlatform::InterfaceIdAllocator::AllocateId(NetworkAdapter* aInterface)
{
    TUint id = iIdPool.Read();
    iInterfaces.insert(std::pair<TUint, NetworkAdapter*>(id, aInterface));
    return (mDNSInterfaceID)id;
}

void MdnsPlatform::InterfaceIdAllocator::DeallocateId(mDNSInterfaceID aInterfaceId)
{
    TUint64 id = (TUint64)aInterfaceId;
    std::map<TUint, NetworkAdapter*>::iterator it = iInterfaces.find(id);
    if (it != iInterfaces.end()) {
        iInterfaces.erase(it);
        iIdPool.Write(id);
    }
}

mDNSInterfaceID MdnsPlatform::InterfaceIdAllocator::GetIdForAddress(const TIpAddress& aAddress)
{
    std::map<TUint, NetworkAdapter*>::iterator it;
    for (it = iInterfaces.begin(); it != iInterfaces.end(); it++) {
        if (it->second->ContainsAddress(aAddress)) {
            return (mDNSInterfaceID)it->first;
        }
    }
    return mDNSInterface_Any;
}

// MdnsPlatform::MdnsService
MdnsPlatform::MdnsService::MdnsService(mDNS& aMdns)
    : iMdns(aMdns)
    , iAction(eInvalid)
{
}

void MdnsPlatform::MdnsService::Set(MdnsServiceAction aAction, TUint aHandle, ServiceRecordSet& aService, const TChar* aName, const TChar* aType, const mDNSInterfaceID aInterfaceId, TUint aPort, const TChar* aInfo)
{
    iAction = aAction;
    iHandle = aHandle;
    iService = &aService;

    if(aName==NULL)
    {
        iName.Replace(Brx::Empty());
    }
    else
    {
        // truncate the name if necessary
        Brn name(aName);
        TUint maxBytes = iName.MaxBytes()-1; // leave room for NULL terminator

        if (name.Bytes()>maxBytes)
        {
            name.Set((TByte*)aName, maxBytes);
        }

        iName.Replace(name);
    }

    iType.Replace((aType == NULL) ? "" : aType);
    iInterfaceId = aInterfaceId;
    iPort = aPort;
    iInfo.Replace((aInfo == NULL) ? "" : aInfo);
}

TUint MdnsPlatform::MdnsService::PerformAction()
{
    switch (iAction)
    {
    case eRegister:
        return Register();
    case eDeregister:
        return Deregister();
    case eRenameAndReregister:
        return RenameAndReregister();
    case eInvalid:
    default:
        ASSERTS();
        return 0;
    }
}

TUint MdnsPlatform::MdnsService::Register()
{
    domainlabel name;
    domainname type;
    domainname domain;
    domainname host;
    mDNSIPPort port;
    SetDomainLabel(name, iName.PtrZ());
    SetDomainName(type, iType.PtrZ());
    SetDomainName(domain, "local");
    SetDomainName(host, "");
    SetPort(port, iPort);
    return mDNS_RegisterService(&iMdns, iService, &name, &type, &domain, 0, port, NULL, (const mDNSu8*)iInfo.PtrZ(), (mDNSu16)strlen(iInfo.PtrZ()), 0, 0, iInterfaceId, &MdnsPlatform::ServiceCallback, this, 0);
}

TUint MdnsPlatform::MdnsService::Deregister()
{
    return mDNS_DeregisterService(&iMdns, iService);
}

TUint MdnsPlatform::MdnsService::RenameAndReregister()
{
    domainlabel name;
    SetDomainLabel(name, iName.PtrZ());
    return mDNS_RenameAndReregisterService(&iMdns, iService, &name);
}


// MdnsPlatform::MdnsEventScheduler

MdnsPlatform::MdnsEventScheduler::MdnsEventScheduler(Environment& aStack, mDNS* aMdns)
    : iMdns(aMdns)
    , iNextEvent(kEventInvalid)
    , iEnabled(true)
    , iLock("MEVT")
{
    iTimer = new Timer(aStack, MakeFunctor(*this, &MdnsPlatform::MdnsEventScheduler::TimerExpired), "MdnsEventScheduler");
}

MdnsPlatform::MdnsEventScheduler::~MdnsEventScheduler()
{
    delete iTimer;
}

void MdnsPlatform::MdnsEventScheduler::TrySchedule(TInt aEvent)
{
    AutoMutex amx(iLock);
    if (aEvent < kEventInvalid) {
        THROW(MdnsImpossibleEvent);
    }
    else if (aEvent == iNextEvent) {
        THROW(MdnsDuplicateEvent);
    }
    else {
        iNextEvent = aEvent;
        if (iEnabled) {
            iTimer->FireAt(iNextEvent);
        }
    }
}

TBool MdnsPlatform::MdnsEventScheduler::Enabled()
{
    AutoMutex amx(iLock);
    return iEnabled;
}

void MdnsPlatform::MdnsEventScheduler::SetEnabled(TBool aEnable)
{
    AutoMutex amx(iLock);
    iEnabled = aEnable;
}

void MdnsPlatform::MdnsEventScheduler::TimerExpired()
{
    if (!mDNS_Execute(iMdns)) {
        LOG_ERROR(kBonjour, "Bonjour             Call to mDNS_Execute() failed. Retrying...\n");
        Log::Print("MdnsPlatform::MdnsEventScheduler::TimerExpired() Call to mDNS_Execute() failed. Retrying...\n");
        ScheduleNow();
    }
}

void MdnsPlatform::MdnsEventScheduler::ScheduleNow()
{
    try {
        TrySchedule(mDNSPlatformRawTime() + kEventRetryMs);
    }
    catch (MdnsDuplicateEvent&) {
        // Can occur in some scenarios. Indicates normal operation will resume on the following event.
        Log::Print("MdnsPlatform::MdnsEventScheduler::ScheduleNow() Caught MdnsDuplicateEvent\n");
    }
    catch (MdnsImpossibleEvent&) {
        Log::Print("MdnsPlatform::MdnsEventScheduler::ScheduleNow() FAILURE: Attempt to retry mDNS_Execute() failed\n");
        throw;
    }
}

// MulticastListener::ReadWriteLock

MulticastListener::ReadWriteLock::ReadWriteLock() 
    : iReaderCount(0)
    , iLockReaders("RWLR")
    , iLockWriter("RWLW")
{
}

void MulticastListener::ReadWriteLock::AcquireReadLock()
{
    AutoMutex amx(iLockReaders);
    iReaderCount++;
    if (iReaderCount == 1) {
        iLockWriter.Wait();
    }
}

void MulticastListener::ReadWriteLock::ReleaseReadLock()
{
    AutoMutex amx(iLockReaders);
    ASSERT(iReaderCount > 0);
    iReaderCount--;
    if (iReaderCount == 0) {
        iLockWriter.Signal();
    }
}

void MulticastListener::ReadWriteLock::AcquireWriteLock()
{
    iLockWriter.Wait();
}

void MulticastListener::ReadWriteLock::ReleaseWriteLock()
{
    iLockWriter.Signal();
}


// MulticastListener

const TUint MulticastListener::kMaxMessageBytes;
const Brn MulticastListener::kAddressV4("224.0.0.251");
const Brn MulticastListener::kAddressV6("ff02::fb");

MulticastListener::MulticastListener(Environment& aEnv, IMdnsMulticastPacketReceiver& aReceiver)
    : iMulticast()
    , iEnv(aEnv)
    , iReceiver(aReceiver)
    , iReader(NULL)
    , iReaderController(NULL)
    , iMulticastLock()
    , iSemReader("MLSR", 0)
    , iThreadListen(NULL)
    , iStop(false)
    , iLock("MLLL")
{
    iThreadListen = new ThreadFunctor("MulticastListener", MakeFunctor(*this, &MulticastListener::ThreadListen));
}

MulticastListener::~MulticastListener()
{
    // ::Stop() must have been called prior to this.
    ASSERT(iThreadListen == NULL);
    ASSERT(iStop == true);
    delete iReaderController;
    delete iReader;
}

void MulticastListener::Start()
{
    ASSERT(iThreadListen != NULL);
    iThreadListen->Start();
}

void MulticastListener::Stop()
{
    ASSERT(iThreadListen != NULL);

    {
        AutoMutex amx(iLock);
        iStop = true;
    }

    {
        iMulticastLock.AcquireReadLock();
        if (iReader != NULL) {
            iReaderController->ReadInterrupt();
            iReader->Interrupt(true);
        }
        iMulticastLock.ReleaseReadLock();
    }
    iSemReader.Signal();

    iThreadListen->Kill();
    delete iThreadListen;
    iThreadListen = NULL;
}

void MulticastListener::Clear()
{
    iMulticastLock.AcquireReadLock();
    if (iReader != NULL) {
        iReaderController->ReadInterrupt();
        iReader->Interrupt(true);
    }
    iMulticastLock.ReleaseReadLock();


    iMulticastLock.AcquireWriteLock();
    // iReader no longer available. Clear any signals.
    iSemReader.Clear();

    delete iReaderController;
    iReaderController = NULL;
    delete iReader;
    iReader = NULL;
    iMulticastLock.ReleaseWriteLock();
}

void MulticastListener::Bind(const TIpAddress& aAddress)
{
    // Throws NetworkError if unable to listen for multicast on aAddress.
    {
        AutoMutex amx(iLock);
        ASSERT(!iStop);
    }

    Endpoint::AddressBuf addressBuf;
    TIpAddressUtils::ToString(aAddress, addressBuf);

    const Brn bindAddr = (aAddress.iFamily == kFamilyV6) ? kAddressV6 : kAddressV4;
    Endpoint epBind(kListenPort, bindAddr);
    iMulticast.Replace(epBind);

    iMulticastLock.AcquireWriteLock();

    // This must be first call to Bind(), or Clear() must have been called prior to this.
    ASSERT(iReader == NULL);
    ASSERT(iReaderController == NULL);

    LOG(kBonjour, "MulticastListener::Bind aAddress: %.*s\n", PBUF(addressBuf));
    try {
        iReader = new SocketUdpMulticast(iEnv, aAddress, iMulticast);
        iReaderController = new UdpReader(*iReader);
        iSemReader.Signal();
        LOG(kBonjour, "MulticastListener::Bind successfully created multicast socket on %.*s\n", PBUF(addressBuf));
    }
    catch (const NetworkError&) {
        LOG(kBonjour, "MulticastListener::Bind NetworkError creating multicast socket on %.*s\n", PBUF(addressBuf));
        delete iReaderController;
        iReaderController = NULL;
        delete iReader;
        iReader = NULL;
        iMulticastLock.ReleaseWriteLock();

        throw;
    }

    iMulticastLock.ReleaseWriteLock();
}

void MulticastListener::ThreadListen()
{
    LOG(kBonjour, "MulticastListener::ThreadListen\n");

    TBool waitOnReady = false;

    while (!iStop) {

        if (waitOnReady) {
            iSemReader.Wait();
            waitOnReady = false;
        }

        iMulticastLock.AcquireReadLock();
        if (iReader == NULL) {
            waitOnReady = true;
        }
        else {
            try {
                LOG(kBonjour, "MulticastListener::ThreadListen - Wait For Message\n");
                iReaderController->Read(iMessage);
                LOG(kBonjour, "MulticastListener::ThreadListen - Message Received\n");
                const Endpoint src = iReaderController->Sender();
                iReceiver.ReceiveMulticastPacket(iMessage, src, iMulticast);

                iReaderController->ReadFlush();
            }
            catch (ReaderError) {
                if (!iStop) {
                    LOG(kBonjour, "MulticastListener::ThreadListen - Reader Error\n");
                }
            }
        }
        iMulticastLock.ReleaseReadLock();
    }
}


// MulticastListeners

MulticastListeners::MulticastListeners(Environment& aEnv, IMdnsMulticastPacketReceiver& aReceiver)
    : iEnv(aEnv)
    , iReceiver(aReceiver)
    , iStarted(false)
    , iStopped(false)
    , iLock("MULL")
{
}

MulticastListeners::~MulticastListeners()
{
    AutoMutex amx(iLock);
    ASSERT(iStopped);
    for (TUint i=0; i<iListeners.size(); i++) {
        delete iListeners[i];
    }
}

void MulticastListeners::Start()
{
    AutoMutex amx(iLock);
    for (TUint i=0; i<iListeners.size(); i++) {
        iListeners[i]->Start();
    }
    iStarted = true;
}

void MulticastListeners::Stop()
{
    AutoMutex amx(iLock);
    for (TUint i=0; i<iListeners.size(); i++) {
        iListeners[i]->Stop();
    }
    iStopped = true;
}

void MulticastListeners::Rebind(std::vector<NetworkAdapter*>& aAdapters)
{
    // For ease, clear all current listeners and rebind later, if appropriate.
    AutoMutex amx(iLock);
    ClearListenersLocked();

    TUint nextListenerIdx = 0;
    for (TUint i = 0; i < aAdapters.size(); i++)
    {
        const TIpAddress adapterAddress = aAdapters[i]->Address();
        Endpoint::AddressBuf addressBuf;
        TIpAddressUtils::ToString(adapterAddress, addressBuf);

        LOG(kBonjour, "MulticastListeners::Rebind aAdapters.size(): %u, i: %u, addr: %.*s\n", aAdapters.size(), i, PBUF(addressBuf));

        if (AdapterIsSuitableListener(adapterAddress))
        {
            ASSERT(nextListenerIdx <= iListeners.size());
            if (nextListenerIdx == iListeners.size())
            {
                // We have a suitable adapter, but no Listeners to bind against, create a new one
                LOG(kBonjour, "MulticastListeners::Rebind Creating new listener. nextListenerIdx: %u\n", nextListenerIdx);
                CreateListenerLocked();
            }

            try
            {
                // Attempt to bind using available listener    
                iListeners[nextListenerIdx]->Bind(adapterAddress);
                nextListenerIdx++;
            }
            catch (const NetworkError&)
            {
                LOG(kBonjour, "MulticastListeners::Rebind NetworkError creating multicast socket on %.*s\n", PBUF(addressBuf));
                ClearListenersLocked();
                throw;
            }
        }
    }
}

void MulticastListeners::CreateListenerLocked()
{
    MulticastListener* listener = new MulticastListener(iEnv, iReceiver);
    if (iStarted && !iStopped) {
        listener->Start();
    }
    iListeners.push_back(listener);
}

void MulticastListeners::ClearListenersLocked()
{
    for (TUint i=0; i<iListeners.size(); i++) {
        iListeners[i]->Clear();
    }
}

TBool MulticastListeners::AdapterIsSuitableListener(const TIpAddress& aAdapter)
{
    // Non-loopback adapter and if IPv6 then must be link-local
    if (!TIpAddressUtils::IsLoopback(aAdapter))
    {
        if (aAdapter.iFamily == kFamilyV4)
        {
            return true;
        }
        else if (TIpAddressUtils::IsLinkLocalIPv6Address(aAdapter))
        {
            return true;
        }
    }
    return false;
}


// MdnsPlatform

const TChar* MdnsPlatform::kNifCookie = "Bonjour";

MdnsPlatform::MdnsPlatform(Environment& aEnv, const TChar* aHost, TBool aHasCache)
    : iEnv(aEnv)
    , iHost(aHost)
    , iHasCache(aHasCache)
    , iListeners(iEnv, *this)
    , iIPv6Enabled(iEnv.InitParams()->IPv6Supported())
    , iClient(aEnv, 5353, iIPv6Enabled ? eSocketFamilyV6 : eSocketFamilyV4)
    , iInterfacesLock("BNJ2")
    , iInterfaceIdAllocator()
    , iServicesLock("BNJ3")
    , iFifoFree(kMaxQueueLength)
    , iFifoPending(kMaxQueueLength)
    , iSem("BNJS", 0)
    , iStop(false)
    , iMdnsCache(NULL)
    , iDiscoveryLock("BNJ6")
    , iMulticastReceiveLock("BNJ7")
{
    LOG(kBonjour, "Bonjour             Constructor\n");
    iNextServiceIndex = 0;
    iMdns = &mDNSStorage;
    iEventScheduler = new MdnsEventScheduler(iEnv, iMdns);

    Status status = mStatus_NoError;
    if (iHasCache) {
        iMdnsCache = (CacheEntity*)calloc(kRRCacheSize, sizeof *iMdnsCache);
        status = mDNS_Init(iMdns, (mDNS_PlatformSupport*)this, iMdnsCache, kRRCacheSize, mDNS_Init_AdvertiseLocalAddresses,
                 StatusCallback, mDNS_Init_NoInitCallbackContext);
    }
    else {
        status = mDNS_Init(iMdns, (mDNS_PlatformSupport*)this, mDNS_Init_NoCache, mDNS_Init_ZeroCacheSize, mDNS_Init_AdvertiseLocalAddresses,
                 StatusCallback, mDNS_Init_NoInitCallbackContext);
    }
    LOG(kBonjour, "Bonjour             Init Status %d\n", status);
    ASSERT(status >= 0);
    LOG(kBonjour, "Bonjour             Init - Start listener thread\n");
    iListeners.Start();
    LOG(kBonjour, "Bonjour             Constructor completed\n");

    for (TUint i=0; i<kMaxQueueLength; i++) {
        iFifoFree.Write(new MdnsService(*iMdns));
    }

    LOG(kBonjour, "Bonjour             Init - Start service thread\n");
    iThreadService = new ThreadFunctor("MdnsServiceThread", MakeFunctor(*this, &MdnsPlatform::ServiceThread));
    iThreadService->Start();
}

MdnsPlatform::~MdnsPlatform()
{
    iEnv.NetworkAdapterList().RemoveSubnetListChangeListener(iSubnetListChangeListenerId);
    iEnv.NetworkAdapterList().RemoveCurrentChangeListener(iCurrentAdapterChangeListenerId);
    iEventScheduler->SetEnabled(false);

    for (TUint i=0; i<iSdRefs.size(); i++) {
        DNSServiceRefDeallocate(*iSdRefs[i]);
    }

    mDNS_Close(iMdns);
    Map::iterator it = iServices.begin();
    while (it != iServices.end()) {
        delete it->second;
        it++;
    }
    for (TUint i=0; i<(TUint)iInterfaces.size(); i++) {
        iInterfaceIdAllocator.DeallocateId(iInterfaces[i]->Info().InterfaceID);
        delete iInterfaces[i];
    }
    for (TUint i=0; i<iDynamicCache.size(); i++) {
        free(iDynamicCache[i]);
    }
    iFifoFree.ReadInterrupt(true);
    iFifoFree.ReadInterrupt(false);
    while (iFifoFree.SlotsUsed() > 0) {
        delete iFifoFree.Read();
    }
    iFifoPending.ReadInterrupt(false);
    while (iFifoPending.SlotsUsed() > 0) {
        delete iFifoPending.Read();
    }
    delete iEventScheduler;
    free(iMdnsCache);
}

void MdnsPlatform::TimerExpired()
{
    LOG(kBonjour, "Bonjour             Timer Expired\n");
    mDNS_Execute(iMdns);
}

void MdnsPlatform::SubnetListChanged()
{
    UpdateInterfaceList();
}

void MdnsPlatform::CurrentAdapterChanged()
{
    UpdateInterfaceList();
}

void MdnsPlatform::UpdateInterfaceList()
{
    iInterfacesLock.Wait();
    NetworkAdapterList& nifList = iEnv.NetworkAdapterList();
    std::vector<NetworkAdapter*>* subnetList = nifList.CreateSubnetList();

    // Check to see if any interfaces are no longer available
    for (TInt i=(TInt)iInterfaces.size()-1; i>=0; i--) {
        if (InterfaceIndex(iInterfaces[i]->Adapter(), *subnetList) == -1) {
            mDNS_DeregisterInterface(iMdns, &iInterfaces[i]->Info(), NormalActivation);
            iInterfaceIdAllocator.DeallocateId(iInterfaces[i]->Info().InterfaceID);
            delete iInterfaces[i];
            iInterfaces.erase(iInterfaces.begin()+i);
        }
    }

    // Add any new interfaces
    AddValidInterfaces(*subnetList);
    iInterfacesLock.Signal();

    iListeners.Rebind(*subnetList);     // May throw NetworkError.
    nifList.DestroySubnetList(subnetList);
}

MdnsPlatform::Status MdnsPlatform::AddValidInterfaces(std::vector<NetworkAdapter*>& aSubnetList)
{
    Status status = mStatus_NoError;
    for (TUint i = 0; i < (TUint)aSubnetList.size() && status == mStatus_NoError; i++)
    {
        if ((InterfaceIndex(*(aSubnetList[i])) == -1) &&
            InterfaceIsValid(aSubnetList[i]->Address()))
        {
            status = AddInterface(aSubnetList[i]);
        }
    }
    return status;
}

TBool MdnsPlatform::InterfaceIsValid(const TIpAddress& aInterface)
{
    if (!TIpAddressUtils::IsLoopback(aInterface))
    {
        if (aInterface.iFamily == kFamilyV4)
        {
            return true;
        }
        else if (TIpAddressUtils::IsLinkLocalIPv6Address(aInterface))
        {
            return true;
        }
    }
    return false;
}

MdnsPlatform::Status MdnsPlatform::AddInterface(NetworkAdapter* aNif)
{
    Status status;
    NetworkInterfaceInfo* nifInfo = (NetworkInterfaceInfo*)calloc(1, sizeof(*nifInfo));
    nifInfo->InterfaceID = iInterfaceIdAllocator.AllocateId(aNif);

    SetAddress(nifInfo->ip, Endpoint(0, aNif->Address()));
    SetAddress(nifInfo->mask, Endpoint(0, aNif->Mask()));
    size_t len = strlen(aNif->Name());
    if (len > 64) { // max length of mDNS' interface name
        len = 64;
    }
    strncpy(nifInfo->ifname, aNif->Name(), len);
    nifInfo->Advertise = mDNStrue;
    nifInfo->McastTxRx = mDNStrue;
    status = mDNS_RegisterInterface(iMdns, nifInfo, NormalActivation);
    if (status == mStatus_NoError) {
        iInterfaces.push_back(new MdnsPlatform::Nif(*aNif, nifInfo));
    }
    else {
        free(nifInfo);
    }
    return status;
}

TInt MdnsPlatform::InterfaceIndex(const NetworkAdapter& aNif)
{
    for (TUint i=0; i<(TUint)iInterfaces.size(); i++) {
        if (NifsMatch(iInterfaces[i]->Adapter(), aNif)) {
            return i;
        }
    }
    return -1;
}

TInt MdnsPlatform::InterfaceIndex(const NetworkAdapter& aNif, const std::vector<NetworkAdapter*>& aList)
{
    for (TUint i=0; i<(TUint)aList.size(); i++) {
        if (NifsMatch(*(aList[i]), aNif)) {
            return i;
        }
    }
    return -1;
}

TBool MdnsPlatform::NifsMatch(const NetworkAdapter& aNif1, const NetworkAdapter& aNif2)
{
    if (TIpAddressUtils::Equals(aNif1.Address(), aNif2.Address()) &&
        TIpAddressUtils::Equals(aNif1.Subnet(), aNif2.Subnet()) &&
        strcmp(aNif1.Name(), aNif2.Name()) == 0) {
        return true;
    }
    return false;
}

void MdnsPlatform::ReceiveMulticastPacket(const Brx& aMsg, const Endpoint aSrc, const Endpoint aDst)
{
    mDNSAddr dst;
    mDNSIPPort dstport;
    SetAddress(dst, aDst);
    SetPort(dstport, aDst);

    mDNSAddr src;
    mDNSIPPort srcport;

    TByte* ptr = (TByte*)aMsg.Ptr();
    TUint bytes = aMsg.Bytes();
    SetAddress(src, aSrc);
    SetPort(srcport, aSrc);

    mDNSInterfaceID interfaceId = (mDNSInterfaceID)0;
    {
        AutoMutex amx(iInterfacesLock);
        TIpAddress senderAddr = aSrc.Address();
        for (TUint i=0; i<(TUint)iInterfaces.size(); i++) {
            if (iInterfaces[i]->ContainsAddress(senderAddr)) {
                interfaceId = iInterfaces[i]->Info().InterfaceID;
                break;
            }
        }
    }

    if (interfaceId != (mDNSInterfaceID)0) {
        AutoMutex amx(iMulticastReceiveLock);
        DNSMessage* msg = reinterpret_cast<DNSMessage*>(ptr);
        mDNSCoreReceive(iMdns, msg, ptr + bytes, &src, srcport, &dst, dstport, interfaceId);
    }
}

void MdnsPlatform::ServiceThread()
{
    /* mDNS_Register, mDNS_Deregister and mDNS_RenameAndReregister calls are
     * all asynchronous.
     *
     * We need to ensure one call on a service record has been completed before
     * we initiate another call. Otherwise, if we try deregister and register a
     * service the register call may fail as we could still be waiting on the
     * deregister call to respond.
     *
     * From profiling, calls to register can take ~600ms and calls to
     * deregister can take ~4000ms before the callback is made, so we store a
     * queue of pending calls and have a thread that processes them in order.
     */
    while (!iStop) {
        try {
            Log::Print("MdnsPlatform::ServiceThread - read fifo\n");
            MdnsService* service = iFifoPending.Read();
            Log::Print("MdnsPlatform::ServiceThread - perform action (%p)\n", service);
            TUint status = service->PerformAction();
            Log::Print("MdnsPlatform::ServiceThread - performed action\n");
            iFifoFree.Write(service);
            if (status == mStatus_NoError) {
                Log::Print("MdnsPlatform::ServiceThread - waiting\n");
                iSem.Wait();
            }
        }
        catch (FifoReadError&) {
            Log::Print("MdnsPlatform::ServiceThread - caught (ignored) FifoReadError\n");
        }
    }
}

void MdnsPlatform::SetAddress(mDNSAddr& aAddress, const Endpoint& aEndpoint)
{
    LOG(kBonjour, "Bonjour             SetAddress ");

    if (aEndpoint.Address().iFamily == kFamilyV4) {
        aAddress.type = mDNSAddrType_IPv4;
        TByte ipv4Octets[4];
        aEndpoint.GetAddressOctets(ipv4Octets);
        aAddress.ip.v4.b[0] = ipv4Octets[0];
        aAddress.ip.v4.b[1] = ipv4Octets[1];
        aAddress.ip.v4.b[2] = ipv4Octets[2];
        aAddress.ip.v4.b[3] = ipv4Octets[3];
    }
    else {
        aAddress.type = mDNSAddrType_IPv6;
        for (TUint i = 0; i < 16; i++) {
            aAddress.ip.v6.b[i] = aEndpoint.Address().iV6[i];
        }
    }

    Endpoint::AddressBuf addrBuf;
    aEndpoint.AppendAddress(addrBuf);
    LOG(kBonjour, "%.*s\n", PBUF(addrBuf));
}

void MdnsPlatform::SetPort(mDNSIPPort& aPort, const Endpoint& aEndpoint)
{
    LOG(kBonjour, "Bonjour             SetPort From Endpoint\n");
    SetPort(aPort, aEndpoint.Port());
}

void MdnsPlatform::SetPort(mDNSIPPort& aPort, TUint aValue)
{
    LOG(kBonjour, "Bonjour             SetPort %d\n", aValue);
    aPort.NotAnInteger = Arch::BigEndian2((TUint16)aValue);
}

void MdnsPlatform::SetDomainLabel(domainlabel& aLabel, const TChar* aBuffer)
{
    LOG(kBonjour, "Bonjour             SetDomainLabel: %s\n", aBuffer);
    MakeDomainLabelFromLiteralString(&aLabel, aBuffer);
    LOG(kBonjour, "Bonjour             SetDomainLabel Length: %d\n", aLabel.c[0]);
}

void MdnsPlatform::SetDomainName(domainname& aName, const TChar* aBuffer)
{
    LOG(kBonjour, "Bonjour             SetDomainName: %s\n", aBuffer);
    MakeDomainNameFromDNSNameString(&aName, aBuffer);
    LOG(kBonjour, "Bonjour             SetDomainName Length: %d\n", aName.c[0]);
}

void MdnsPlatform::SetHostName(const TChar* aName)
{
    iHost.Set(aName);
    void DoSetHostName();
}

void MdnsPlatform::DoSetHostName()
{
    if (iHost.Bytes() > 0) {
        SetDomainLabel(iMdns->nicelabel, (const TChar*)iHost.Ptr());
        SetDomainLabel(iMdns->hostlabel, (const TChar*)iHost.Ptr());
    }
    else {
        SetDomainLabel(iMdns->nicelabel, "");
        SetDomainLabel(iMdns->hostlabel, "");
    }
    mDNS_SetFQDN(iMdns);
}

TUint MdnsPlatform::CreateService()
{
    LOG(kBonjour, "Bonjour             CreateService\n");
    ServiceRecordSet* service = new ServiceRecordSet();
    iServicesLock.Wait();
    TUint handle = iNextServiceIndex;
    iServices.insert(std::pair<TUint, ServiceRecordSet*>(handle, service));
    iNextServiceIndex++;
    iServicesLock.Signal();
    LOG(kBonjour, "Bonjour             CreateService - Complete\n");
    return handle;
}

void MdnsPlatform::DeregisterService(TUint aHandle)
{
    LOG(kBonjour, "Bonjour             DeregisterService\n");
    iServicesLock.Wait();
    Map::iterator it = iServices.find(aHandle);
    if (it != iServices.end()) {
        MdnsService* mdnsService;
        try {
            mdnsService = iFifoFree.Read();
        }
        catch (FifoReadError&) {
            iServicesLock.Signal();
            return;
        }
        mdnsService->Set(eDeregister, aHandle, *it->second, NULL, NULL, 0, 0, NULL);
        iFifoPending.Write(mdnsService);
    }
    iServicesLock.Signal();
    LOG(kBonjour, "Bonjour             DeregisterService - Complete\n");
}

void MdnsPlatform::RegisterService(TUint aHandle, const TChar* aName, const TChar* aType, const TIpAddress& aInterface, TUint aPort, const TChar* aInfo)
{
    LOG(kBonjour, "Bonjour             RegisterService\n");
    iServicesLock.Wait();
    Map::iterator it = iServices.find(aHandle);
    ASSERT(it != iServices.end());
    ServiceRecordSet* service = it->second;
    iServicesLock.Signal();
    MdnsService* mdnsService;

    try {
        mdnsService = iFifoFree.Read();
    }
    catch (FifoReadError&) {
        return;
    }
    mDNSInterfaceID interfaceId = iInterfaceIdAllocator.GetIdForAddress(aInterface);
    mdnsService->Set(eRegister, aHandle, *service, aName, aType, interfaceId, aPort, aInfo);
    iFifoPending.Write(mdnsService);

    LOG(kBonjour, "Bonjour             RegisterService - Complete\n");
}

void MdnsPlatform::RenameAndReregisterService(TUint aHandle, const TChar* aName)
{
    LOG(kBonjour, "Bonjour             RenameAndReregisterService\n");
    iServicesLock.Wait();
    ServiceRecordSet* service = iServices[aHandle];
    iServicesLock.Signal();
    MdnsService* mdnsService;

    try {
        mdnsService = iFifoFree.Read();
    }
    catch (FifoReadError) {
        return;
    }
    mdnsService->Set(eRenameAndReregister, aHandle, *service, aName, NULL, 0, 0, NULL);
    iFifoPending.Write(mdnsService);

    LOG(kBonjour, "Bonjour             RenameAndReregisterService - Complete\n");
}

void MdnsPlatform::ServiceCallback(mDNS* m, ServiceRecordSet* aRecordSet, mStatus aStatus)
{
    LOG(kBonjour, "Bonjour             ServiceCallback - aRecordSet: %p, aStatus: %d\n", aRecordSet, aStatus);

    MdnsPlatform& platform = *(MdnsPlatform*) m->p;
    platform.iSem.Signal();
}

void MdnsPlatform::Lock()
{
    iMutex.Lock();
    LOG(kBonjour, "Bonjour             Lock\n");
}

void MdnsPlatform::Unlock()
{
    TInt next = iMdns->NextScheduledEvent - iMdns->timenow_adjust;
    try {
        iEventScheduler->TrySchedule(next);
        LOG(kBonjour, "Bonjour             Next Scheduled Event %d\n", next);
    }
    catch (MdnsImpossibleEvent&) {
        LOG(kBonjour, "Bonjour             Ignore Impossible Event: %d\n", next);
    }
    catch (MdnsDuplicateEvent&) {
        LOG(kBonjour, "Bonjour             Ignore Duplicate Event %d\n", next);
    }
    LOG(kBonjour, "Bonjour             Unlock\n");
    iMutex.Unlock();
}

MdnsPlatform::Status MdnsPlatform::Init()
{
    LOG(kBonjour, "Bonjour             Init\n");
    LOG(kBonjour, "Bonjour             Init - Set FQDN\n");
    DoSetHostName();
    LOG(kBonjour, "Bonjour             Init - Register Interface\n");

    iInterfacesLock.Wait();
    Status status = mStatus_NoError;
    NetworkAdapterList& nifList = iEnv.NetworkAdapterList();
    Functor functorSubnet = MakeFunctor(*this, &MdnsPlatform::SubnetListChanged);
    iSubnetListChangeListenerId = nifList.AddSubnetListChangeListener(functorSubnet, "MdnsPlatform-subnet");
    Functor functorAdapter = MakeFunctor(*this, &MdnsPlatform::CurrentAdapterChanged);
    iCurrentAdapterChangeListenerId = nifList.AddCurrentChangeListener(functorAdapter, "MdnsPlatform-current");

    // Subnet list changed and current adapter changed callbacks do not happen on registration, so set up interface list here.
    std::vector<NetworkAdapter*>* subnetList = nifList.CreateSubnetList();
    status = AddValidInterfaces(*subnetList);

    // Attempt initial bind of multicast adapters, as subnet list changed and current adapter changed callbacks do not happen on registration.
    // This will throw NetworkAdapter if there is a valid (non-localhost) adapter which is unable to listen for multicast.
    // Allow exception to bubble up, as can't use mDNS without listening for multicast!
    iListeners.Rebind(*subnetList);
    nifList.DestroySubnetList(subnetList);

    iInterfacesLock.Signal();
    if (status == mStatus_NoError) {
        mDNSCoreInitComplete(iMdns, status);
    }
    return status;
}

MdnsPlatform::Status MdnsPlatform::GetPrimaryInterface(mDNSAddr* aInterfaceV4, mDNSAddr* aInterfaceV6, mDNSAddr* /*aRouter*/)
{
    LOG(kBonjour, "Bonjour             GetPrimaryInterface ");
    Status status = mStatus_NoError;
    TIpAddress addr = kIpAddressV4AllAdapters;
    iInterfacesLock.Wait();
    if (iInterfaces.size() == 0) {
        status = mStatus_NotInitializedErr;
        aInterfaceV4->ip.v4.NotAnInteger = kIpAddressV4AllAdapters.iV4;
        for (TUint j = 0; j < 16; j++) {
            aInterfaceV6->ip.v6.b[j] = 0;
        }
    }
    if (status != mStatus_NotInitializedErr) {
        NetworkAdapter* current = iEnv.NetworkAdapterList().CurrentAdapter(kNifCookie).Ptr();
        if (current == NULL) {
            // We don't have a default adapter, but there is at least one interface stored in iInterfaces, use the first interface
            current = &iInterfaces[0]->Adapter();
        }
        addr = current->Address();

        // Go through the interface list, if the interface name matches our current adapter then supply that as the primary interface. Check for IPv6 on the same adapter.
        for (TUint i = 0; i < iInterfaces.size(); i++) {
            NetworkAdapter& adapter = iInterfaces[i]->Adapter();
            if (strcmp(adapter.Name(), current->Name()) == 0) {
                if (adapter.Address().iFamily == kFamilyV4) {
                    aInterfaceV4->type = mDNSAddrType_IPv4;
                    aInterfaceV4->ip.v4.NotAnInteger = adapter.Address().iV4;
                }
                else if (adapter.Address().iFamily == kFamilyV6) {
                    aInterfaceV6->type = mDNSAddrType_IPv6;
                    for (TUint j = 0; j < 16; j++) {
                        aInterfaceV6->ip.v6.b[j] = adapter.Address().iV6[j];
                    }
                }
            }
        }
        // Require at least the primary interface we selected (current or iInterfaces[0]->Adapter()) to be valid
        if (TIpAddressUtils::IsZero(current->Address())) {
            status = mStatus_NotInitializedErr;
            aInterfaceV4->ip.v4.NotAnInteger = kIpAddressV4AllAdapters.iV4;
            for (TUint j = 0; j < 16; j++) {
                aInterfaceV6->ip.v6.b[j] = 0;
            }
        }
    }
    iInterfacesLock.Signal();

    Bws<Endpoint::kMaxAddressBytes> addrBuf;
    TIpAddressUtils::ToString(addr, addrBuf);
    LOG(kBonjour, addrBuf);
    LOG(kBonjour, "\n");

    return status;
}

MdnsPlatform::Status MdnsPlatform::SendUdp(const Brx& aBuffer, const Endpoint& aEndpoint)
{
    LOG(kBonjour, "Bonjour             SendUdp\n");
    iClient.Send(aBuffer, aEndpoint);
    return 0;
}

void MdnsPlatform::Close()
{
    ASSERT(!iEventScheduler->Enabled());
    iStop = true;
    iListeners.Stop();

    iThreadService->Kill();
    iFifoPending.ReadInterrupt(true);
    iSem.Signal();
    delete iThreadService;
}

void MdnsPlatform::AppendTxtRecord(Bwx& aBuffer, const TChar* aKey, const TChar* aValue)
{
    ASSERT((strlen(aKey) + strlen(aValue) + 3) <= (aBuffer.MaxBytes()-aBuffer.Bytes()));
    TByte length = (TByte)(strlen(aKey) + strlen(aValue) + 1);
    aBuffer.Append(length);
    aBuffer.Append(aKey);
    aBuffer.Append('=');
    aBuffer.Append(aValue);
}

extern "C" 
void ResolveReply(
    DNSServiceRef       /*sdRef*/,
    DNSServiceFlags     flags,
    uint32_t            interfaceIndex,
    DNSServiceErrorType errorCode,
    const char          *fullname,
    const char          *hosttarget,
    uint16_t            port,        /* In network byte order */
    const unsigned char *ipAddr,
    const char          *regtype,
    uint16_t            txtLen,
    const unsigned char *txtRecord,
    void                *context)
{
    if (errorCode == kDNSServiceErr_NoError) {
        Brn friendlyName(fullname);
        Brn devtype(regtype);
        Brn uglyName(hosttarget);
        Bws<20> ip;
        ip.AppendPrintf("%d.%d.%d.%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);
        Bwh text(txtLen);
        TUint8 length = 0;
        for (const unsigned char* ptr = txtRecord; ptr < txtRecord + txtLen; ptr += length) {
            length = *ptr;
            if (ptr > txtRecord) {
                text.Append(' ');
            }
            text.Append(++ptr, length);
        }
        LOG(kBonjour, "mDNS Device discovered: %.*s, target=%.*s, ip=%.*s, port=%d, text=%.*s\n", PBUF(friendlyName), PBUF(uglyName), PBUF(ip), port, PBUF(text));
        MdnsPlatform& platform = *(MdnsPlatform*)context;
        platform.DeviceDiscovered(devtype, friendlyName, uglyName, ip, port);
    }
    else {
        LOG_ERROR(kBonjour, "mDNS resolve reply: flags=%d, index=%u, err=%d, fullname=%s, hosttarget=%s, txtRecord=%s, context=%p, port=%d, txtLen=%d\n",
               flags, interfaceIndex, (TInt)errorCode, fullname, hosttarget, txtRecord, context, port, txtLen);
    }
}

extern "C" 
void BrowseReply(DNSServiceRef sdRef,
    DNSServiceFlags      flags,
    uint32_t             interfaceIndex,
    DNSServiceErrorType  errorCode,
    const char           *serviceName,
    const char           *regtype,
    const char           *replyDomain,
    void                 *context)
{
    if (errorCode == kDNSServiceErr_NoError) {      
        LOG(kBonjour, "mDNS Browse Reply (%s): %s\n", regtype, serviceName);         
    }
    else {
        LOG_ERROR(kBonjour, "mDNS browse Error: flags=%d, index=%u, err=%d, serviceName=%s, regtype=%s, replyDomain=%s, context=%p\n",
               flags, interfaceIndex, (TInt)errorCode, serviceName, regtype, replyDomain, context);
    }    

    DNSServiceErrorType err = DNSServiceResolve(&sdRef,
                                                flags,
                                                interfaceIndex,
                                                serviceName,
                                                regtype,
                                                replyDomain,
                                                (DNSServiceResolveReply)ResolveReply,
                                                context);
    if (err != kDNSServiceErr_NoError) {
        LOG_ERROR(kBonjour, "DNSServiceResolve returned error code %d\n", (TInt)err);
    }
}

TBool MdnsPlatform::FindDevices(const TChar* aServiceName)
{
    if (!iHasCache) {
        LOG_ERROR(kBonjour, "ERROR: Mdns cache is required for MdnsPlatform::FindDevices. See Env.InitParams.SetDvEnableBonjour\n");
        ASSERTS();
        return false;
    }
    iDiscoveryLock.Wait();
    DNSServiceRef* ref = new DNSServiceRef();
    iSdRefs.push_back(ref);
    iDiscoveryLock.Signal();
    DNSServiceErrorType err = DNSServiceBrowse(ref,
                                               0, /*flags */
                                               0, /*interfaceIndex -- not used (defaults to mDNSInterface_Any instead) */
                                               aServiceName, /*regtype*/
                                               NULL, /*domain*/
                                               (DNSServiceBrowseReply)BrowseReply,
                                               this /*context*/);
    return (err == kDNSServiceErr_NoError);
}

void MdnsPlatform::DeviceDiscovered(const Brx& aType, const Brx& aFriendlyName, const Brx& aUglyName, const Brx&  aIpAddress, TUint aPort)
{
    AutoMutex _(iDiscoveryLock);
    MdnsDevice* dev = new MdnsDevice(aType, aFriendlyName, aUglyName, aIpAddress, aPort);
    for (TUint i=0; i<iDeviceListeners.size(); i++) {
        
        iDeviceListeners[i]->DeviceAdded(*dev);
    }
    delete dev;
}

void MdnsPlatform::StatusCallback(mDNS *const m, mStatus aStatus)
{
    LOG(kBonjour, "Bonjour             StatusCallback - aStatus %d\n", aStatus);
    if (aStatus == mStatus_GrowCache && reinterpret_cast<MdnsPlatform*>(m->p)->iHasCache) {
        // Allocate another chunk of cache storage
        (void)m;
        LOG(kBonjour, "WARNING: mDNS cache size insufficient, GROWING...\n");
        CacheEntity *storage = (CacheEntity*)calloc(MdnsPlatform::kRRCacheSize, sizeof(CacheEntity));
        if (storage) {
            mDNS_GrowCache(m, storage, MdnsPlatform::kRRCacheSize);
            reinterpret_cast<MdnsPlatform*>(m->p)->iDynamicCache.push_back(storage);
        }
    }
    else if (aStatus != mStatus_NoError) {
        Log::Print("ERROR: mDNS status=%d\n", aStatus);
        ASSERTS();
    }
}

void MdnsPlatform::AddMdnsDeviceListener(IMdnsDeviceListener* aListener)
{
    // could not use std::reference_wrapper for iDeviceListeners
    // pointer is not owned here
    AutoMutex _(iDiscoveryLock);
    iDeviceListeners.push_back(aListener);
}


// MdnsPlatform::MutexRecursive

MdnsPlatform::MutexRecursive::MutexRecursive()
    : iMutex("MREC")
    , iOwner(Thread_None)
    , iCount(0)
{
}

MdnsPlatform::MutexRecursive::~MutexRecursive()
{
    ASSERT(iOwner == Thread_None);
    ASSERT(iCount == 0);
}

void MdnsPlatform::MutexRecursive::Lock()
{
    Thread* th = Thread::Current();
    if (th == iOwner) {
        iCount++;
    }
    else {
        iMutex.Wait();
        iOwner = th;
        iCount = 1;
    }
}

void MdnsPlatform::MutexRecursive::Unlock()
{
    ASSERT_DEBUG(Thread::Current() == iOwner);
    if (--iCount == 0) {
        iOwner = Thread_None;
        iMutex.Signal();
    }
}


// C APIs expected by mDNSCore

extern "C" {

mStatus mDNSPlatformInit(mDNS* m)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformInit\n");
    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    return platform.Init();
}

void mDNSPlatformClose(mDNS* m)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformClose\n");
    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    platform.Close();
}

mStatus mDNSPlatformSendUDP(const mDNS* m, const void* const aMessage, const mDNSu8* const aEnd,
        mDNSInterfaceID aInterface, UDPSocket* /*src*/, const mDNSAddr *aAddress, mDNSIPPort aPort, mDNSBool /*useBackgroundTrafficClass*/)
{
    if (aInterface ==  mDNSInterface_LocalOnly) {
        LOG(kBonjour, "Bonjour             mDNSPlatformSendUDP - local only, ignore\n");
        return 0;
    }

    LOG(kBonjour, "Bonjour             mDNSPlatformSendUDP\n");

    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    Brn buffer((const TByte*)aMessage, (TUint)((const TByte*)aEnd - (const TByte*)aMessage));
    ASSERT(aAddress->type == mDNSAddrType_IPv4 || aAddress->type == mDNSAddrType_IPv6);


    Endpoint::AddressBuf address;
    if (aAddress->type == mDNSAddrType_IPv4) {
        address.AppendPrintf("%d.%d.%d.%d",
            aAddress->ip.v4.b[0],
            aAddress->ip.v4.b[1],
            aAddress->ip.v4.b[2],
            aAddress->ip.v4.b[3] );
        }
    else {
        TIpAddress addr;
        addr.iFamily= kFamilyV6;
        for (TUint i = 0; i < 16; i++) {
            addr.iV6[i] = aAddress->ip.v6.b[i];
        }
        TIpAddressUtils::ToString(addr, address);
    }

    Endpoint endpoint(Arch::BigEndian2(aPort.NotAnInteger), address);
    mStatus status;
    try{
        status = platform.SendUdp(buffer, endpoint);
    }
    catch (NetworkError&)
    {
        LOG_ERROR(kBonjour, "mDNSPlatformSendUDP caught NetworkError. Endpoint port %u, address: ", aPort.NotAnInteger);
        LOG_ERROR(kBonjour, address);
        LOG_ERROR(kBonjour, "\n");
        status = mStatus_UnknownErr;
    }
    return status;
}

void* mDNSPlatformMemAllocate(mDNSu32 aLength)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemAllocate(%u)\n", aLength);
    return malloc(aLength);
}

void* mDNSPlatformMemAllocateClear(mDNSu32 aLength)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemAllocateClear(%u)\n", aLength);
    return malloc(aLength);  
}

void  mDNSPlatformMemFree(void* aPtr)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemFree\n");
    free(aPtr);
}

mDNSInterfaceID mDNSPlatformInterfaceIDfromInterfaceIndex(mDNS* /*m*/, mDNSu32 /*aIndex*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformInterfaceIDfromInterfaceIndex\n");
    // All interfaces registered in mDNS platform are general purpose for advertising/receiving on, return mDNSInterface_Any
    return mDNSInterface_Any;
}

mDNSu32 mDNSPlatformInterfaceIndexfromInterfaceID(mDNS* /*m*/, mDNSInterfaceID /*aId*/, mDNSBool /*suppressNetworkChange*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformInterfaceIndexFromInterfaceID\n");
    // All interfaces registered in mDNS platform are general purpose for advertising/receiving on, return kDNSServiceInterfaceIndexAny
    return kDNSServiceInterfaceIndexAny;
}

// mDNS core calls this routine when it wants to prevent
// the platform from reentering mDNS core code.

void mDNSPlatformLock(const mDNS* m)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformLock\n");
    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    platform.Lock();
}

// mDNS core calls this routine when it release the lock taken by
// mDNSPlatformLock and allow the platform to reenter mDNS core code.

void mDNSPlatformUnlock (const mDNS* m)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformUnlock\n");
    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    platform.Unlock();
}

// mDNS core calls this routine to copy C strings.
// On the Posix platform this maps directly to the ANSI C strcpy.

void mDNSPlatformStrCopy(void *dst, const void *src)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformStrCopy\n");
    strcpy((char*)dst, (char*)src);
}

// mDNS core calls this routine to get the length of a C string.
// On the Posix platform this maps directly to the ANSI C strlen.

mDNSu32 mDNSPlatformStrLen(const void *src)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformStrLen\n");
    return (mDNSu32)strlen((const char*)src);
}

// mDNS core calls this routine to copy memory.
// On the Posix platform this maps directly to the ANSI C memcpy.

void mDNSPlatformMemCopy(void *dst, const void *src, mDNSu32 len)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemCopy\n");
    memcpy(dst, src, len);
}

// mDNS core calls this routine to test whether blocks of memory are byte-for-byte
// identical. On the Posix platform this is a simple wrapper around ANSI C memcmp.

mDNSBool mDNSPlatformMemSame(const void *src, const void *dst, mDNSu32 len)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemSame\n");
    return (memcmp(dst, src, len) == 0);
}

// mDNS core calls this routine to clear blocks of memory.
// On the Posix platform this is a simple wrapper around ANSI C memset.

void mDNSPlatformMemZero(void *dst, mDNSu32 len)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemZero\n");
    memset(dst, 0, len);
}

// If the caller wants to know the exact return of memcmp, then use this instead
// of mDNSPlatformMemSame
int mDNSPlatformMemCmp(const void *dst, const void *src, mDNSu32 len)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformMemCmp\n");
    return (memcmp(dst, src, len));
}

void mDNSPlatformQsort(void *base, int nel, int width, int (*compar)(const void *, const void *))
{
    LOG(kBonjour, "Bonjour             mDNSPlatformQsort\n");
    return (qsort(base, nel, width, compar));
}

// // DNSSEC stub functions
// void VerifySignature(mDNS *const /*m*/, DNSSECVerifier* /*dv*/, DNSQuestion* /*q*/)
// {
// }

// void BumpDNSSECStats(mDNS *const /*m*/, DNSSECStatsAction /*action*/, DNSSECStatsType /*type*/, mDNSu32 /*value*/)
// {
// }

// void StartDNSSECVerification(mDNS *const /*m*/, void* /*context*/)
// {
// }

// RRVerifier* AllocateRRVerifier(const ResourceRecord *const rr, mStatus *status)
// {
//     return NULL;
// }

// mStatus AddRRSetToVerifier(DNSSECVerifier *dv, const ResourceRecord *const rr, RRVerifier *rv, RRVerifierSet set)
// {
//     return mStatus_NoError;
// }

// void FreeDNSSECVerifier(mDNS *const /*m*/, DNSSECVerifier* /*dv*/)
// {
// }

// DNSSECVerifier *AllocateDNSSECVerifier(mDNS *const m, const domainname *name, mDNSu16 rrtype, mDNSInterfaceID InterfaceID, mDNSu8 ValidationRequired, DNSSECVerifierCallback dvcallback, mDNSQuestionCallback qcallback)
// {
//     return NULL;
// }

// void InitializeQuestion(mDNS *const /*m*/, DNSQuestion* /*question*/, mDNSInterfaceID /*InterfaceID*/, const domainname* /*qname*/, mDNSu16 /*qtype*/, mDNSQuestionCallback* /*callback*/, void* /*context*/)
// {
// }

// void ValidateRRSIG(DNSSECVerifier* /*dv*/, RRVerifierSet /*type*/, const ResourceRecord *const /*rr*/)
// {
// }

// void AuthChainLink(DNSSECVerifier* /*dv*/, AuthChain* /*ae*/)
// {
// }

// int DNSSECCanonicalOrder(const domainname *const /*d1*/, const domainname *const /*d2*/, int* /*subdomain*/)
// {
//     return 0;
// }

// int DNSMemCmp(const mDNSu8 *const m1, const mDNSu8 *const m2, int len)
// {
//     int res = 0;
// 	if (memcmp( m1, m2, len ) != 0) {
//         return (res < 0 ? -1 : 1);
//     }
//     return res;
// }

// void ProveInsecure(mDNS *const /*m*/, DNSSECVerifier* /*dv*/, InsecureContext* /*ic*/, domainname* /*trigger*/)
// {
// }

// void DNSSECProbe(mDNS *const /*m*/)
// {
//     // DarwinOS specific function
//     ASSERTS();
// }

// Proxy stub functions
mDNSu8 *DNSProxySetAttributes(DNSQuestion* /*q*/, DNSMessageHeader* /*h*/, DNSMessage* /*msg*/, mDNSu8* /*ptr*/, mDNSu8* /*limit*/)
{
    return NULL;
}

// Logging/debugging

#ifdef DEFINE_TRACE
int mDNS_LoggingEnabled = 1;
int mDNS_PacketLoggingEnabled= 1;
int mDNS_McastTracingEnabled = 1;
#else
int mDNS_LoggingEnabled = 0;
int mDNS_PacketLoggingEnabled= 0;
int mDNS_McastTracingEnabled = 0;
#endif

static const TUint kMaxLogMsgBytes = 200;

void LogMsgWithLevel(mDNSLogCategory_t /*category*/, mDNSLogLevel_t /*logLevel*/, const char *format, ...)
{
#ifdef DEFINE_TRACE
    va_list args;
    va_start(args, format);
    // not all messages are errors but enough are that its handy to log everything here if we're interested in errors
    if(Debug::TestLevel(Debug::kBonjour) && Debug::TestSeverity(Debug::kSeverityError)) {
        Bws<kMaxLogMsgBytes> msg;
        TUint written = mDNS_vsnprintf((char*)msg.Ptr(), msg.MaxBytes(), format, args);
        msg.SetBytes(written);
        Log::Print(msg);
        Log::Print("\n");
    }
    va_end(args);
#endif
}

mDNSu32 mDNSPlatformRandomNumber()
{
    LOG(kBonjour, "Bonjour             mDNSPlatformRandomNumber\n");
    return gEnv->Random();
}

mDNSu32 mDNSPlatformRandomSeed()
{
    LOG(kBonjour, "Bonjour             mDNSPlatformRandomSeed\n");
    return gEnv->Random();
}

// Time handlers

mDNSs32  mDNSPlatformOneSecond = 1000;

mStatus mDNSPlatformTimeInit()
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTimeInit\n");
    return mStatus_NoError;
}

mDNSs32  mDNSPlatformRawTime()
{
    TUint time = Os::TimeInMs(OpenHome::gEnv->OsCtx());
    LOG(kBonjour, "Bonjour             mDNSPlatformRawTime: %d\n", time);
    return time;
}

mDNSs32 mDNSPlatformUTC()
{
    TUint time = (Os::TimeInMs(OpenHome::gEnv->OsCtx()) / 1000) + 1229904000; // 1st Jan 2009
    LOG(kBonjour, "Bonjour             mDNSPlatformUTC: %d\n", time);
    return time;
}

// TCP handlers

TCPSocket* mDNSPlatformTCPSocket(TCPSocketFlags /*flags*/, mDNSAddr_Type /*addrtype*/, mDNSIPPort */*port*/, domainname */*hostname*/, mDNSBool /*useBackgroundTrafficClass*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTCPSocket\n");
    ASSERTS();
    return NULL;
}

TCPSocket* mDNSPlatformTCPAccept(TCPSocketFlags /*flags*/, int /*sd*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTCPAccept\n");
    ASSERTS();
    return NULL;
}

int mDNSPlatformTCPGetFD(TCPSocket* /*sock*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTCPGetFD\n");
    ASSERTS();
    return 0;
}

mStatus mDNSPlatformTCPConnect(TCPSocket* /*sock*/, const mDNSAddr* /*dst*/, mDNSOpaque16 /*dstport*/,
                               mDNSInterfaceID /*InterfaceID*/, TCPConnectionCallback /*callback*/, void* /*context*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTCPConnect\n");
    ASSERTS();
    return mStatus_UnsupportedErr;
}

void mDNSPlatformTCPCloseConnection(TCPSocket*)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTCPCloseConnection\n");
    ASSERTS();
}

long mDNSPlatformReadTCP(TCPSocket* /*sock*/, void* /*buf*/, unsigned long /*buflen*/, mDNSBool* /*closed*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformReadTCP\n");
    ASSERTS();
    return 0;
}

long mDNSPlatformWriteTCP(TCPSocket* /*sock*/, const char* /*msg*/, unsigned long /*len*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformWriteTCP\n");
    ASSERTS();
    return 0;
}

// unused UDP handlers

UDPSocket* mDNSPlatformUDPSocket(const mDNSIPPort /*requestedport*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformUDPSocket\n");
    ASSERTS();
    return NULL;
}

void mDNSPlatformUDPClose(UDPSocket* /*sock*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformUDPClose\n");
    ASSERTS();
}

// unused misc socket handlers

void mDNSPlatformReceiveBPF_fd(int /*fd*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformReceiveBPF_fd\n");
    ASSERTS();
}

void mDNSPlatformUpdateProxyList(const mDNSInterfaceID /*InterfaceID*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformUpdateProxyList\n");
    ASSERTS();
}

void mDNSPlatformSendRawPacket(const void* const /*msg*/, const mDNSu8* const /*end*/, mDNSInterfaceID /*InterfaceID*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformSendRawPacket\n");
    ASSERTS();
}

void mDNSPlatformSetLocalAddressCacheEntry(const mDNSAddr *const /*tpa*/, const mDNSEthAddr *const /*tha*/, mDNSInterfaceID /*InterfaceID*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformSetLocalAddressCacheEntry\n");
    ASSERTS();
}

void mDNSPlatformSourceAddrForDest(mDNSAddr* const /*src*/, const mDNSAddr* const /*dst*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformSourceAddrForDest\n");
    ASSERTS();
}

// dnsextd handlers

mStatus mDNSPlatformTLSSetupCerts()
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTLSSetupCerts\n");
    ASSERTS();
    return mStatus_UnsupportedErr;
}

void mDNSPlatformTLSTearDownCerts()
{
    LOG(kBonjour, "Bonjour             mDNSPlatformTLSTearDownCerts\n");
    ASSERTS();
}

// Handlers for unicast browsing/dynamic update for clients who do not specify a domain
// in browse/registration

mDNSBool mDNSPlatformSetDNSConfig(mDNSBool /*setservers*/, mDNSBool /*setsearch*/, domainname* const fqdn,
                              DNameListElem** /*RegDomains*/, DNameListElem** /*BrowseDomains*/, mDNSBool /*ackConfig*/)

{
    // unused, but called by Bonjour
    LOG(kBonjour, "Bonjour             mDNSPlatformSetDNSConfig\n");
    if (fqdn != mDNSNULL) {
        (void)memset(fqdn, 0, sizeof(*fqdn));
    }
    return true;
}

mStatus mDNSPlatformGetPrimaryInterface(mDNS* const m, mDNSAddr* v4, mDNSAddr* v6, mDNSAddr* router)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformGetPrimaryInterface\n");
    mStatus err = mStatus_NoError;
    MdnsPlatform& platform = *(MdnsPlatform*)m->p;
    err = platform.GetPrimaryInterface(v4, v6, router);

    return err;
}

void mDNSPlatformDynDNSHostNameStatusChanged(const domainname* const /*dname*/, const mStatus /*status*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformDynDNSHostNameStatusChanged\n");
    ASSERTS();
}

void mDNSPlatformSetAllowSleep(mDNSBool /*allowSleep*/, const char* /*reason*/)
{
    // unused, but called by Bonjour
    LOG(kBonjour, "Bonjour             mDNSPlatformSetAllowSleep\n");
}

void mDNSPlatformSendWakeupPacket(mDNSInterfaceID /*InterfaceID*/, char* /*EthAddr*/, char* /*IPAddr*/, int /*iteration*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformSendWakeupPacket\n");
    ASSERTS();
}

mDNSBool mDNSPlatformValidRecordForInterface(const AuthRecord* /*rr*/, mDNSInterfaceID /*InterfaceID*/)
{
    LOG(kBonjour, "Bonjour             mDNSPlatformValidRecordForInterface\n");
    return true;
}

mDNSBool mDNSPlatformValidQuestionForInterface(DNSQuestion* /*q*/, const NetworkInterfaceInfo* /*intf*/)
{
    return true;
}

// Used for debugging purposes. For now, just set the buffer to zero
void mDNSPlatformFormatTime(unsigned long /*te*/, mDNSu8 *buf, int bufsize)
{
    if (bufsize) buf[0] = 0;
}

void mDNSPlatformSendKeepalive(mDNSAddr* /*sadd*/, mDNSAddr* /*dadd*/, mDNSIPPort* /*lport*/, mDNSIPPort* /*rport*/, mDNSu32 /*seq*/, mDNSu32 /*ack*/, mDNSu16 /*win*/)
{

}

mStatus mDNSPlatformRetrieveTCPInfo(mDNSAddr *laddr, mDNSIPPort* /*lport*/, mDNSAddr* /*raddr*/, mDNSIPPort* /*rport*/, mDNSTCPInfo* /*mti*/)
{
    return mStatus_NoError;
}

mStatus mDNSPlatformGetRemoteMacAddr(mDNSAddr* /*raddr*/)
{
    return mStatus_NoError;
}

mStatus mDNSPlatformStoreSPSMACAddr(mDNSAddr* /*spsaddr*/, char* /*ifname*/)
{
    return mStatus_NoError;
}

mStatus mDNSPlatformClearSPSData(void)
{
    return mStatus_NoError;
}

mStatus mDNSPlatformStoreOwnerOptRecord(char* /*ifname*/, DNSMessage* /*msg*/, int /*length*/)
{
    return mStatus_UnsupportedErr;
}

mDNSBool mDNSPlatformInterfaceIsD2D(mDNSInterfaceID /*InterfaceID*/)
{
    return mDNSfalse;
}

void mDNSPlatformSetSocktOpt(void* /*sock*/, mDNSTransport_Type /*transType*/, mDNSAddr_Type /*addrType*/, const DNSQuestion* /*q*/)
{

}

mDNSs32 mDNSPlatformGetPID()
{
    return 0;
}


} // extern "C"

