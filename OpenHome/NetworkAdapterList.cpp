#include <OpenHome/Private/NetworkAdapterList.h>
#include <OpenHome/Types.h>
#include <OpenHome/Exception.h>
#include <OpenHome/OsWrapper.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Env.h>
#include <OpenHome/Exception.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Private/TIpAddressUtils.h>

#include <algorithm>
#include <vector>
#include <map>

using namespace OpenHome;

// NetworkAdapterList

NetworkAdapterList::NetworkAdapterList(Environment& aEnv, Environment::ELoopback aLoopbackPolicy, TBool aIPv6Supported, const TIpAddress& aDefaultSubnet)
    : iEnv(aEnv)
    , iLoopbackPolicy(aLoopbackPolicy)
    , iListLock("MNIL")
    , iListenerLock("MNIO")
    , iCurrent(NULL)
    , iNextListenerId(1)
    , iSingleSubnetMode(false)
    , iIPv6Supported(aIPv6Supported)
{
    iEnv.AddObject(this);
    iEnv.AddResumeObserver(*this);
    iDefaultSubnet = aDefaultSubnet;
    iNotifierThread = new NetworkAdapterChangeNotifier(*this);
    iNotifierThread->Start();
    iNetworkAdapters = Os::NetworkListAdapters(iEnv, iLoopbackPolicy, iIPv6Supported, "NetworkAdapterList");
    iSubnets = CreateSubnetList();
    Os::NetworkSetInterfaceChangedObserver(iEnv.OsCtx(), &InterfaceListChanged, this);
    for (size_t i=0; i<iSubnets->size(); i++) {
        TraceAdapter("NetworkAdapter added", *(*iSubnets)[i]);
    }
}

NetworkAdapterList::~NetworkAdapterList()
{
    iEnv.RemoveResumeObserver(*this);
    delete iNotifierThread;
    DestroySubnetList(iNetworkAdapters);
    DestroySubnetList(iSubnets);
    iEnv.RemoveObject(this);
}

TBool NetworkAdapterList::SingleSubnetModeEnabled() const
{
    AutoMutex a(iListLock);
    return iSingleSubnetMode;
}

Optional<NetworkAdapter> NetworkAdapterList::CurrentAdapter(const char* aCookie) const
{
    AutoMutex a(iListLock);
    if (iCurrent == NULL) {
        return NULL;
    }
    iCurrent->AddRef(aCookie);
    return iCurrent;
}

std::vector<NetworkAdapter*>* NetworkAdapterList::CreateSubnetList() const
{
    iListLock.Wait();
    std::vector<NetworkAdapter*>* list = CreateSubnetListLocked();
    iListLock.Signal();
    return list;
}

void NetworkAdapterList::DestroySubnetList(std::vector<NetworkAdapter*>* aList)
{
    if (aList != NULL) {
        for (TUint i=0; i<aList->size(); i++) {
            (*aList)[i]->RemoveRef("NetworkAdapterList");
        }
        delete aList;
    }
}

std::vector<NetworkAdapter*>* NetworkAdapterList::CreateNetworkAdapterList() const
{
    iListLock.Wait();

    std::vector<NetworkAdapter*>* list = new std::vector<NetworkAdapter*>;
    for (TUint i=0; i<iNetworkAdapters->size(); i++) {
        NetworkAdapter* nif = (*iNetworkAdapters)[i];
        nif->AddRef("NetworkAdapterList");
        list->push_back(nif);
    }

    iListLock.Signal();
    return list;
}

void NetworkAdapterList::DestroyNetworkAdapterList(std::vector<NetworkAdapter*>* aList)
{
    DestroySubnetList(aList);
}

void NetworkAdapterList::SetCurrentSubnet(const TIpAddress& aSubnet)
{
    iListLock.Wait();
    iSingleSubnetMode = !TIpAddressUtils::Equals(aSubnet, kIpAddressV4AllAdapters);
    const TIpAddress oldAddress = (iCurrent==NULL ? kIpAddressV4AllAdapters : iCurrent->Address());
    iDefaultSubnet = aSubnet;
    UpdateCurrentAdapter();
    const TIpAddress newAddress = (iCurrent==NULL? kIpAddressV4AllAdapters : iCurrent->Address());
    iListLock.Signal();
    const TBool started = (iEnv.CpiStack() != NULL || iEnv.DviStack() != NULL);
    if (started && !TIpAddressUtils::Equals(newAddress, oldAddress)) {
        iNotifierThread->QueueCurrentChanged();
    }
}

void NetworkAdapterList::SetIPv6Supported(TBool aIPv6Supported)
{
    if (iIPv6Supported != aIPv6Supported)
    {
        iIPv6Supported = aIPv6Supported;
        Refresh();
    }
}

void NetworkAdapterList::Refresh()
{
    HandleInterfaceListChanged();
}

TUint NetworkAdapterList::AddCurrentChangeListener(Functor aFunctor, const TChar* aId, TBool aInternalClient)
{
    if (aInternalClient) {
        return AddListener(aFunctor, aId, iListenersCurrentInternal);
    }
    return AddListener(aFunctor, aId, iListenersCurrentExternal);
}

void NetworkAdapterList::RemoveCurrentChangeListener(TUint aId)
{
    if (!RemoveSubnetListChangeListener(aId, iListenersCurrentInternal)) {
        (void)RemoveSubnetListChangeListener(aId, iListenersCurrentExternal);
    }
}

TUint NetworkAdapterList::AddSubnetListChangeListener(Functor aFunctor, const TChar* aId, TBool aInternalClient)
{
    if (aInternalClient) {
        return AddListener(aFunctor, aId, iListenersSubnetInternal);
    }
    return AddListener(aFunctor, aId, iListenersSubnetExternal);
}

void NetworkAdapterList::RemoveSubnetListChangeListener(TUint aId)
{
    if (!RemoveSubnetListChangeListener(aId, iListenersSubnetInternal)) {
        (void)RemoveSubnetListChangeListener(aId, iListenersSubnetExternal);
    }
}

TUint NetworkAdapterList::AddSubnetAddedListener(FunctorNetworkAdapter aFunctor, const TChar* aId)
{
    return AddSubnetListener(aFunctor, aId, iListenersAdded);
}

void NetworkAdapterList::RemoveSubnetAddedListener(TUint aId)
{
    RemoveSubnetListener(aId, iListenersAdded);
}

TUint NetworkAdapterList::AddSubnetRemovedListener(FunctorNetworkAdapter aFunctor, const TChar* aId)
{
    return AddSubnetListener(aFunctor, aId, iListenersRemoved);
}

void NetworkAdapterList::RemoveSubnetRemovedListener(TUint aId)
{
    RemoveSubnetListener(aId, iListenersRemoved);
}

TUint NetworkAdapterList::AddNetworkAdapterChangeListener(FunctorNetworkAdapter aFunctor, const TChar* aId)
{
    return AddSubnetListener(aFunctor, aId, iListenersAdapterChanged);
}

void NetworkAdapterList::RemoveNetworkAdapterChangeListener(TUint aId)
{
    RemoveSubnetListener(aId, iListenersAdapterChanged);
}

// Disabled while we experiment with retrying RunCallbacks() instead
/*
void NetworkAdapterList::TempFailureRetry(Functor& aCallback)
{ // static
    static const TUint kDelaysMs[] = { 100, 200, 400, 800, 1600, 3200, 5000, 10000, 20000, 20000, 30000 }; // roughly 90s worth of retries
    for (TUint i=0; i<sizeof(kDelaysMs)/sizeof(kDelaysMs[0]); i++) {
        try {
            aCallback();
            return;
        }
        catch (NetworkError&) {
            LOG_ERROR(kNetwork, "TempFailureRetry: error handling adapter change, try again in %ums\n", kDelaysMs[i]);
            Thread::Sleep(kDelaysMs[i]);
        }
    }
    THROW(NetworkError);
}
*/
std::vector<NetworkAdapter*>* NetworkAdapterList::CreateSubnetListLocked() const
{
    std::vector<NetworkAdapter*>* list = new std::vector<NetworkAdapter*>;
    for (TUint i=0; i<iNetworkAdapters->size(); i++) {
        NetworkAdapter* nif = (*iNetworkAdapters)[i];
        TIpAddress subnet = nif->Subnet();
        if (-1 == NetworkAdapterList::FindSubnet(subnet, *list)) {
            nif->AddRef("NetworkAdapterList");
            list->push_back(nif);
        }
    }
    return list;
}

TUint NetworkAdapterList::AddListener(Functor aFunctor, const TChar* aId, VectorListener& aList)
{
    iListenerLock.Wait();
    TUint id = iNextListenerId;
    Listener listener(aFunctor, aId);
    aList.push_back(std::pair<TUint, Listener>(id, listener));
    iNextListenerId++;
    iListenerLock.Signal();
    return id;
}

TBool NetworkAdapterList::RemoveSubnetListChangeListener(TUint aId, VectorListener& aList)
{
    TBool removed = false;
    iListenerLock.Wait();
    VectorListener::iterator it = aList.begin();
    while (it != aList.end()) {
        if (it->first == aId) {
            aList.erase(it);
            removed = true;
            break;
        }
        it++;
    }
    iListenerLock.Signal();
    return removed;
}

TUint NetworkAdapterList::AddSubnetListener(FunctorNetworkAdapter aFunctor, const TChar* aId, MapNetworkAdapter& aMap)
{
    iListenerLock.Wait();
    TUint id = iNextListenerId;
    ListenerNetworkAdapter listener(aFunctor, aId);
    aMap.insert(std::pair<TUint, ListenerNetworkAdapter>(id, listener));
    iNextListenerId++;
    iListenerLock.Signal();
    return id;
}

void NetworkAdapterList::RemoveSubnetListener(TUint aId, MapNetworkAdapter& aMap)
{
    iListenerLock.Wait();
    MapNetworkAdapter::iterator it = aMap.find(aId);
    if (it != aMap.end()) {
        aMap.erase(it);
    }
    iListenerLock.Signal();
}

void NetworkAdapterList::InterfaceListChanged(void* aPtr)
{
    try
    {
        reinterpret_cast<NetworkAdapterList*>(aPtr)->HandleInterfaceListChanged();
    }
    catch (AssertionFailed&) {
        throw;
    }
    catch(Exception& e) {
        UnhandledExceptionHandler(e);
    }
    catch(std::exception& e) {
        UnhandledExceptionHandler(e);
    }
    catch(...) {
        UnhandledExceptionHandler( "Unknown Exception", "Unknown File", 0 );
    }
}

TInt NetworkAdapterList::FindSubnet(const TIpAddress& aSubnet, const std::vector<NetworkAdapter*>& aList)
{
    for (TUint i=0; i<aList.size(); i++) {
        if (TIpAddressUtils::Equals(aList[i]->Subnet(), aSubnet)) {
            return i;
        }
    }
    return -1;
}

void NetworkAdapterList::UpdateCurrentAdapter()
{
    iCurrent = NULL;
    if (iNetworkAdapters != NULL && iNetworkAdapters->size() > 0) {
        for (TUint i=0; i<iNetworkAdapters->size(); i++) {
            NetworkAdapter* nif = (*iNetworkAdapters)[i];
            if (TIpAddressUtils::Equals(nif->Subnet(), iDefaultSubnet)) {
                iCurrent = nif;
                TraceAdapter("Subnet changed", *iCurrent);
                break;
            }
        }
    }
    if (iCurrent == NULL) {
        LOG_TRACE(kAdapterChange, "Subnet changed: none active\n");
    }
}

TBool NetworkAdapterList::CompareSubnets(NetworkAdapter* aI, NetworkAdapter* aJ)
{
    return TIpAddressUtils::LessThan(aI->Subnet(), aJ->Subnet());
}

void NetworkAdapterList::HandleInterfaceListChanged()
{
    static const char* kRemovedAdapterCookie = "RemovedAdapter";
    iListLock.Wait();
    std::vector<NetworkAdapter*>* list = Os::NetworkListAdapters(iEnv, iLoopbackPolicy, iIPv6Supported, "NetworkAdapterList");
    TIpAddress oldAddress = (iCurrent==NULL ? kIpAddressV4AllAdapters : iCurrent->Address());
    DestroySubnetList(iNetworkAdapters);
    iNetworkAdapters = list;

    // update the 'current' adapter and inform observers if it has changed
    UpdateCurrentAdapter();
    TIpAddress newAddress = (iCurrent==NULL? kIpAddressV4AllAdapters : iCurrent->Address());

    // update the subnet list, noting if it has changed
    std::vector<NetworkAdapter*>* subnets = CreateSubnetListLocked();
    TBool subnetsChanged = false;
    if (subnets->size() != iSubnets->size()) {
        subnetsChanged = true;
    }
    else {
        for (TUint i=0; i<iSubnets->size(); i++) {
            if (!TIpAddressUtils::Equals((*iSubnets)[i]->Address(),  (*subnets)[i]->Address())) {
                subnetsChanged = true;
                break;
            }
        }
    }

    // determine adds and/or removes from list
    std::vector<NetworkAdapter*> oldSubnetsObj(iSubnets->begin(), iSubnets->end());
    std::vector<NetworkAdapter*> newSubnetsObj(subnets->begin(), subnets->end());
    std::vector<NetworkAdapter*>* oldSubnets = &oldSubnetsObj;
    std::vector<NetworkAdapter*>* newSubnets = &newSubnetsObj;
    std::vector<NetworkAdapter*> added;
    std::vector<NetworkAdapter*> removed;
    std::vector<NetworkAdapter*> adapterChanged;

    std::sort(oldSubnets->begin(), oldSubnets->end(), CompareSubnets);
    std::sort(newSubnets->begin(), newSubnets->end(), CompareSubnets);

    if (oldSubnets->size() == 0 && newSubnets->size() > 0) {
        for (TUint i=0; i < newSubnets->size(); i++) {
            added.push_back((*newSubnets)[i]);
        }
    }
    else if (oldSubnets->size() > 0 && newSubnets->size() == 0) {
        for (TUint i=0; i < oldSubnets->size(); i++) {
            NetworkAdapter* removedAdapter = (*oldSubnets)[i];
            removed.push_back(removedAdapter);
            removedAdapter->AddRef(kRemovedAdapterCookie); // DestroySubnetList(iSubnets) may destroy the last ref to this before QueueAdapterRemoved later claims a new ref
        }
    }
    else {
        TUint j = 0;
        for (TUint i=0; i < newSubnets->size(); i++) {
            while (j < oldSubnets->size() && 
                    TIpAddressUtils::LessThan((*oldSubnets)[j]->Subnet(), (*newSubnets)[i]->Subnet())) {
                NetworkAdapter* removedAdapter = (*oldSubnets)[j];
                removed.push_back(removedAdapter);
                removedAdapter->AddRef(kRemovedAdapterCookie);
                j++;
            }
            if (j < oldSubnets->size() &&
                TIpAddressUtils::Equals((*oldSubnets)[j]->Subnet(), (*newSubnets)[i]->Subnet())) {
                if (!TIpAddressUtils::Equals((*oldSubnets)[j]->Address(), (*newSubnets)[i]->Address())) {
                    adapterChanged.push_back((*newSubnets)[i]);
                }
                j++;
            }
        }
        if (j < oldSubnets->size()) {
            while (j < oldSubnets->size()) {
                NetworkAdapter* removedAdapter = (*oldSubnets)[j];
                removed.push_back(removedAdapter);
                removedAdapter->AddRef(kRemovedAdapterCookie);
                j++;
            }
        }
        j = 0;
        for (TUint i=0; i < oldSubnets->size(); i++) {
            while (j < newSubnets->size() &&
                    TIpAddressUtils::LessThan((*newSubnets)[j]->Subnet(), (*oldSubnets)[i]->Subnet())) {
                added.push_back((*newSubnets)[j]);
                j++;
            }
            if (j < newSubnets->size() &&
                TIpAddressUtils::Equals((*newSubnets)[j]->Subnet(), (*oldSubnets)[i]->Subnet())) {
                j++;
            }
        }
        if (j < newSubnets->size()) {
            while (j < newSubnets->size()) {
                added.push_back((*newSubnets)[j]);
                j++;
            }
        }
    }

    DestroySubnetList(iSubnets);
    iSubnets = subnets;
    iListLock.Signal();

    if (subnetsChanged) {
        iNotifierThread->QueueSubnetsChanged();
    }
    if (!TIpAddressUtils::Equals(newAddress, oldAddress)) {
        iNotifierThread->QueueCurrentChanged();
    }

    // Notify added/removed callbacks.
    if (removed.size() > 0) {
        for (TUint i=0; i < removed.size(); i++) {
            NetworkAdapter* removedAdapter = removed[i];
            TraceAdapter("NetworkAdapter removed", *removedAdapter);
            iNotifierThread->QueueAdapterRemoved(*removedAdapter);
            removedAdapter->RemoveRef(kRemovedAdapterCookie);
        }
    }
    if (added.size() > 0) {
        for (TUint i=0; i < added.size(); i++) {
            TraceAdapter("NetworkAdapter added", *added[i]);
            iNotifierThread->QueueAdapterAdded(*added[i]);
        }
    }

    // Notify network adapter changed callbacks.
    if (adapterChanged.size() > 0) {
        for (TUint i=0; i < adapterChanged.size(); i++) {
            TraceAdapter("NetworkAdapter changed", *adapterChanged[i]);
            iNotifierThread->QueueAdapterChanged(*adapterChanged[i]);
        }
    }
}

void NetworkAdapterList::RunCallbacks(const VectorListener& aCallbacks)
{
    static const TUint kDelaysMs[] = { 100, 200, 400, 800, 1600, 3200, 5000, 10000, 20000, 20000, 30000 }; // roughly 90s worth of retries
    // Back off and retry all callbacks if there is a NetworkError. It is up to any previously successful callbacks to determine if they must take any action.
    for (TUint i=0; i<sizeof(kDelaysMs)/sizeof(kDelaysMs[0]); i++) {
        try {
            DoRunCallbacks(aCallbacks);
            return;
        }
        catch (AssertionFailed&) {
            throw;
        }
        catch (Exception& ex) {
            LOG_ERROR(kAdapterChange, "TempFailureRetry: error (%s) handling adapter change, try again in %ums\n", ex.Message(), kDelaysMs[i]);
            Thread::Sleep(kDelaysMs[i]);
        }
    }
    THROW(NetworkError);

}

void NetworkAdapterList::DoRunCallbacks(const VectorListener& aCallbacks)
{
    TBool error = false;
    AutoMutex a(iListenerLock);
    VectorListener::const_iterator it = aCallbacks.begin();
    while (it != aCallbacks.end()) {
        LOG(kAdapterChange, "NetworkAdapterList::DoRunCallbacks - client is %s\n", it->second.iId);
        try {
            it->second.iFunctor();
        }
        catch (const NetworkError&) {
            // Consume NetworkError thrown by a misbehaving callback and run callbacks on remaining listeners so they do not end up in an inconsistent state.
            // Set flag and throw exception at end to notify caller so they can retry callbacks if desired.
            error = true;
        }
        it++;
    }
    if (error) {
        THROW(NetworkError);
    }
}

void NetworkAdapterList::RunSubnetCallbacks(MapNetworkAdapter& aMap, NetworkAdapter& aAdapter)
{
    AutoMutex a(iListenerLock);
    MapNetworkAdapter::iterator it = aMap.begin();
    while (it != aMap.end()) {
        LOG(kAdapterChange, "NetworkAdapterList::RunSubnetCallbacks - client is %s\n", it->second.iId);
        it->second.iFunctor(aAdapter);
        it++;
    }
}

void NetworkAdapterList::TraceAdapter(const TChar* aPrefix, NetworkAdapter& aAdapter)
{ // static
    Endpoint ep(0, aAdapter.Address());
    Bws<Endpoint::kMaxAddressBytes> addr;
    ep.AppendAddress(addr);
    LOG_TRACE(kNetwork, "%s: %s(%s)\n", aPrefix, aAdapter.Name(), (const TChar*)addr.Ptr());
}

void NetworkAdapterList::ListObjectDetails() const
{
    Log::Print("  NetworkAdapterList: addr=%p\n", this);
}

void NetworkAdapterList::NotifyCurrentChanged()
{
    RunCallbacks(iListenersCurrentInternal);
    RunCallbacks(iListenersCurrentExternal);
}

void NetworkAdapterList::NotifySubnetsChanged()
{
    RunCallbacks(iListenersSubnetInternal);
    RunCallbacks(iListenersSubnetExternal);
}

void NetworkAdapterList::NotifyAdapterAdded(NetworkAdapter& aAdapter)
{
    RunSubnetCallbacks(iListenersAdded, aAdapter);
}

void NetworkAdapterList::NotifyAdapterRemoved(NetworkAdapter& aAdapter)
{
    RunSubnetCallbacks(iListenersRemoved, aAdapter);
}

void NetworkAdapterList::NotifyAdapterChanged(NetworkAdapter& aAdapter)
{
    RunSubnetCallbacks(iListenersAdapterChanged, aAdapter);
}

void NetworkAdapterList::NotifyResumed()
{
    HandleInterfaceListChanged();
}


// NetworkAdapterChangeNotifier

NetworkAdapterChangeNotifier::NetworkAdapterChangeNotifier(INetworkAdapterChangeNotifier& aAdapterList)
    : Thread("AdapterChange")
    , iAdapterList(aAdapterList)
    , iLock("NACN")
{
}

NetworkAdapterChangeNotifier::~NetworkAdapterChangeNotifier()
{
    Kill();
    Join();
    while (iList.size() > 0) {
        iLock.Wait();
        UpdateBase* update = iList.front();
        iList.pop_front();
        iLock.Signal();
        delete update;
    }
}

void NetworkAdapterChangeNotifier::QueueCurrentChanged()
{
    Queue(new UpdateCurrent());
}

void NetworkAdapterChangeNotifier::QueueSubnetsChanged()
{
    Queue(new UpdateSubnet());
}

void NetworkAdapterChangeNotifier::QueueAdapterAdded(NetworkAdapter& aAdapter)
{
    Queue(new UpdateAdapterAdded(aAdapter));
}

void NetworkAdapterChangeNotifier::QueueAdapterRemoved(NetworkAdapter& aAdapter)
{
    Queue(new UpdateAdapterRemoved(aAdapter));
}

void NetworkAdapterChangeNotifier::QueueAdapterChanged(NetworkAdapter& aAdapter)
{
    Queue(new UpdateAdapterChanged(aAdapter));
}

void NetworkAdapterChangeNotifier::Queue(UpdateBase* aUpdate)
{
    iLock.Wait();
    iList.push_back(aUpdate);
    iLock.Signal();
    Signal();
}

void NetworkAdapterChangeNotifier::Run()
{
    for (;;) {
        Wait();
        iLock.Wait();
        UpdateBase* update = iList.front();
        iList.pop_front();
        iLock.Signal();
        try {
            update->Update(iAdapterList);
        }
        catch (AssertionFailed&) {
            throw;
        }
        catch (Exception& ex) {
            LOG_ERROR(kAdapterChange, "NetworkAdapterChangeNotifier::Run() exception %s from %s:%u\n",
                                      ex.Message(), ex.File(), ex.Line());
        }
        delete update;
    }
}


// NetworkAdapterChangeNotifier::UpdateBase

NetworkAdapterChangeNotifier::UpdateBase::~UpdateBase()
{
}

NetworkAdapterChangeNotifier::UpdateBase::UpdateBase()
{
}


// NetworkAdapterChangeNotifier::UpdateCurrent

void NetworkAdapterChangeNotifier::UpdateCurrent::Update(INetworkAdapterChangeNotifier& aAdapterList)
{
    aAdapterList.NotifyCurrentChanged();
}


// NetworkAdapterChangeNotifier::UpdateSubnet

void NetworkAdapterChangeNotifier::UpdateSubnet::Update(INetworkAdapterChangeNotifier& aAdapterList)
{
    aAdapterList.NotifySubnetsChanged();
}


// NetworkAdapterChangeNotifier::UpdateAdapter

NetworkAdapterChangeNotifier::UpdateAdapter::UpdateAdapter(NetworkAdapter& aAdapter)
    : iAdapter(aAdapter)
{
    iAdapter.AddRef("NetworkAdapterChangeNotifier::UpdateBase");
}

NetworkAdapterChangeNotifier::UpdateAdapter::~UpdateAdapter()
{
    iAdapter.RemoveRef("NetworkAdapterChangeNotifier::UpdateBase");
}


// NetworkAdapterChangeNotifier::UpdateAdapterAdded

NetworkAdapterChangeNotifier::UpdateAdapterAdded::UpdateAdapterAdded(NetworkAdapter& aAdapter)
    : NetworkAdapterChangeNotifier::UpdateAdapter(aAdapter)
{
}

void NetworkAdapterChangeNotifier::UpdateAdapterAdded::Update(INetworkAdapterChangeNotifier& aAdapterList)
{
    aAdapterList.NotifyAdapterAdded(iAdapter);
}


// NetworkAdapterChangeNotifier::UpdateAdapterRemoved

NetworkAdapterChangeNotifier::UpdateAdapterRemoved::UpdateAdapterRemoved(NetworkAdapter& aAdapter)
    : NetworkAdapterChangeNotifier::UpdateAdapter(aAdapter)
{
}

void NetworkAdapterChangeNotifier::UpdateAdapterRemoved::Update(INetworkAdapterChangeNotifier& aAdapterList)
{
    aAdapterList.NotifyAdapterRemoved(iAdapter);
}


// NetworkAdapterChangeNotifier::UpdateAdapterChanged

NetworkAdapterChangeNotifier::UpdateAdapterChanged::UpdateAdapterChanged(NetworkAdapter& aAdapter)
    : NetworkAdapterChangeNotifier::UpdateAdapter(aAdapter)
{
}

void NetworkAdapterChangeNotifier::UpdateAdapterChanged::Update(INetworkAdapterChangeNotifier& aAdapterList)
{
    aAdapterList.NotifyAdapterChanged(iAdapter);
}
