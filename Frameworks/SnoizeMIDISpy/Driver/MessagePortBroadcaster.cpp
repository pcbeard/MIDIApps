#include "MessagePortBroadcaster.h"

#include "MIDISpyShared.h"
#include <pthread.h>


// NOTE This static variable is a dumb workaround. See comment in MessagePortWasInvalidated().
static MessagePortBroadcaster *sOneBroadcaster = NULL;

MessagePortBroadcaster::MessagePortBroadcaster(CFStringRef broadcasterName, MessagePortBroadcasterDelegate *delegate) :
    mDelegate(delegate),
    mBroadcasterName(NULL),
    mLocalPort(NULL),
    mRunLoopSource(NULL),
    mNextListenerIdentifier(0),
    mListenersByIdentifier(NULL),
    mIdentifiersByListener(NULL),
    mListenerArraysByChannel(NULL)
{
    CFMessagePortContext messagePortContext = { 0, (void *)this, NULL, NULL, NULL };

    sOneBroadcaster = this;
        
    if (!broadcasterName)
        broadcasterName = CFSTR("Unknown Broadcaster");
    mBroadcasterName = CFStringCreateCopy(kCFAllocatorDefault, broadcasterName);

    // Create a local port for remote listeners to talk to us with
    mLocalPort = CFMessagePortCreateLocal(kCFAllocatorDefault, mBroadcasterName, LocalMessagePortCallBack, &messagePortContext, FALSE);

    // And add it to the current run loop
    mRunLoopSource = CFMessagePortCreateRunLoopSource(kCFAllocatorDefault, mLocalPort, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), mRunLoopSource, kCFRunLoopDefaultMode);

    // Create structures to keep track of our listeners
    mListenersByIdentifier = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);
    mIdentifiersByListener = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
    mListenerArraysByChannel = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, &kCFTypeDictionaryValueCallBacks);

    pthread_mutex_init(&mListenerStructuresMutex, NULL);
}

MessagePortBroadcaster::~MessagePortBroadcaster()
{
    sOneBroadcaster = NULL;

    pthread_mutex_destroy(&mListenerStructuresMutex);

    if (mListenerArraysByChannel)
        CFRelease(mListenerArraysByChannel);

    if (mIdentifiersByListener)
        CFRelease(mIdentifiersByListener);

    if (mListenersByIdentifier)
        CFRelease(mListenersByIdentifier);

    if (mRunLoopSource) {
        CFRunLoopSourceInvalidate(mRunLoopSource);
        CFRelease(mRunLoopSource);
    }

    if (mLocalPort) {
        CFMessagePortInvalidate(mLocalPort);
        CFRelease(mLocalPort);
    }

    CFRelease(mBroadcasterName);    
}

void MessagePortBroadcaster::Broadcast(CFDataRef data, SInt32 channel)
{
    CFArrayRef listeners;
    CFIndex listenerIndex;

    pthread_mutex_lock(&mListenerStructuresMutex);

    listeners = (CFArrayRef)CFDictionaryGetValue(mListenerArraysByChannel, (void *)channel);
    if (listeners) {
        listenerIndex = CFArrayGetCount(listeners);
    
        while (listenerIndex--) {
            CFMessagePortRef listenerPort;
    
            listenerPort = (CFMessagePortRef)CFArrayGetValueAtIndex(listeners, listenerIndex);
            CFMessagePortSendRequest(listenerPort, 0, data, 300, 0, NULL, NULL);
        }
    }

    pthread_mutex_unlock(&mListenerStructuresMutex);
}


//
// Private functions and methods
//

CFDataRef LocalMessagePortCallBack(CFMessagePortRef local, SInt32 msgid, CFDataRef data, void *info)
{
    MessagePortBroadcaster *broadcaster = (MessagePortBroadcaster *)info;
    CFDataRef result = NULL;

    switch (msgid) {
        case kSpyingMIDIDriverGetNextListenerIdentifierMessageID:
            result = broadcaster->NextListenerIdentifier();
            break;

        case kSpyingMIDIDriverAddListenerMessageID:
            broadcaster->AddListener(data);
            break;

        case kSpyingMIDIDriverConnectDestinationMessageID:
        case kSpyingMIDIDriverDisconnectDestinationMessageID:
            broadcaster->ChangeListenerChannelStatus(data, (msgid == kSpyingMIDIDriverConnectDestinationMessageID));
            break;

        default:
            break;        
    }

    return result;
}

CFDataRef	MessagePortBroadcaster::NextListenerIdentifier()
{
    // Client is starting up; it wants to know what identifier to use (so it can name its local port).
    // We give it that data in a reply.

    CFDataRef returnedData;

    mNextListenerIdentifier++;
    returnedData = CFDataCreate(kCFAllocatorDefault, (UInt8 *)&mNextListenerIdentifier, sizeof(UInt32));

    return returnedData;
}

void	MessagePortBroadcaster::AddListener(CFDataRef listenerIdentifierData)
{
    // The listener has created a local port on its side, and we need to create a remote port for it.
    // No reply is necessary.

    const UInt8 *dataBytes;
    UInt32 listenerIdentifier;
    CFStringRef listenerPortName;
    CFMessagePortRef remotePort;

    if (!listenerIdentifierData || CFDataGetLength(listenerIdentifierData) != sizeof(UInt32))
        return;

    dataBytes = CFDataGetBytePtr(listenerIdentifierData);
    if (!dataBytes)
        return;

    listenerIdentifier = *(const UInt32 *)dataBytes;
    listenerPortName = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("%@-%lu"), mBroadcasterName, listenerIdentifier);

    remotePort = CFMessagePortCreateRemote(kCFAllocatorDefault, listenerPortName);
    if (remotePort) {
        CFMessagePortSetInvalidationCallBack(remotePort, MessagePortWasInvalidated);

        pthread_mutex_lock(&mListenerStructuresMutex);
        CFDictionarySetValue(mListenersByIdentifier, (void *)listenerIdentifier, (void *)remotePort);
        CFDictionarySetValue(mIdentifiersByListener, (void *)remotePort, (void *)listenerIdentifier);
        pthread_mutex_unlock(&mListenerStructuresMutex);

        CFRelease(remotePort);

        if (mDelegate && CFDictionaryGetCount(mListenersByIdentifier) == 1)
            mDelegate->BroadcasterListenerCountChanged(this, true);
    }

    CFRelease(listenerPortName);
}

void	MessagePortBroadcaster::ChangeListenerChannelStatus(CFDataRef messageData, Boolean shouldAdd)
{
    // From the message data given, take out the identifier of the listener, and the channel it is concerned with.
    // Then find the remote message port corresponding to that identifier.
    // Then find the array of listeners for this channel (creating it if necessary), and add/remove the remote port from the array.
    // No reply is necessary.
    
    const UInt8 *dataBytes;
    UInt32 identifier;
    SInt32 channel;
    CFMessagePortRef remotePort;
    CFMutableArrayRef channelListeners;

    if (!messageData || CFDataGetLength(messageData) != sizeof(UInt32) + sizeof(SInt32))
        return;
    dataBytes = CFDataGetBytePtr(messageData);
    if (!dataBytes)
        return;
    identifier = *(UInt32 *)dataBytes;
    channel = *(SInt32 *)(dataBytes + sizeof(UInt32));

    remotePort = (CFMessagePortRef)CFDictionaryGetValue(mListenersByIdentifier, (void *)identifier);
    if (!remotePort)
        return;

    pthread_mutex_lock(&mListenerStructuresMutex);
        
    channelListeners = (CFMutableArrayRef)CFDictionaryGetValue(mListenerArraysByChannel, (void *)channel);
    if (!channelListeners && shouldAdd) {
        channelListeners = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        CFDictionarySetValue(mListenerArraysByChannel, (void *)channel, channelListeners);
    }

    if (shouldAdd) {
        CFArrayAppendValue(channelListeners, remotePort);
    } else if (channelListeners) {
        CFIndex index;

        index = CFArrayGetFirstIndexOfValue(channelListeners, CFRangeMake(0, CFArrayGetCount(channelListeners)), remotePort);
        if (index != kCFNotFound)
            CFArrayRemoveValueAtIndex(channelListeners, index);
    }

    pthread_mutex_unlock(&mListenerStructuresMutex);
}

void MessagePortWasInvalidated(CFMessagePortRef messagePort, void *info)
{
    // NOTE: The info pointer provided to this function is useless. CFMessagePort provides no way to set it for remote ports.
    // Thus, we have to assume we have one MessagePortBroadcaster, which we look up statically. Lame!
    // TODO come up with a better solution to this

#if DEBUG
    fprintf(stderr, "MessagePortBroadcaster: remote port was invalidated\n");
#endif

    sOneBroadcaster->RemoveListenerWithRemotePort(messagePort);
}

void	MessagePortBroadcaster::RemoveListenerWithRemotePort(CFMessagePortRef remotePort)
{
    UInt32 identifier;

    pthread_mutex_lock(&mListenerStructuresMutex);

    // Remove this listener from our dictionaries
    identifier = (UInt32)CFDictionaryGetValue(mIdentifiersByListener, (void *)remotePort);
    CFDictionaryRemoveValue(mListenersByIdentifier, (void *)identifier);
    CFDictionaryRemoveValue(mIdentifiersByListener, (void *)remotePort);

    // Also go through the listener array for each channel and remove remotePort from there too
    CFDictionaryApplyFunction(mListenerArraysByChannel, RemoveRemotePortFromChannelArray, remotePort);    

    pthread_mutex_unlock(&mListenerStructuresMutex);

    if (mDelegate && CFDictionaryGetCount(mListenersByIdentifier) == 0)
        mDelegate->BroadcasterListenerCountChanged(this, false);    
}

void RemoveRemotePortFromChannelArray(const void *key, const void *value, void *context)
{
    // We don't care about the key (it's a channel number)
    CFMutableArrayRef listenerArray = (CFMutableArrayRef)value;
    CFMessagePortRef remotePort = (CFMessagePortRef)context;
    CFIndex index;

    index = CFArrayGetFirstIndexOfValue(listenerArray, CFRangeMake(0, CFArrayGetCount(listenerArray)), remotePort);
    if (index != kCFNotFound)
        CFArrayRemoveValueAtIndex(listenerArray, index);
}
