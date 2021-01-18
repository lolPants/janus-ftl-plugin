/**
 * @file FtlServer.cpp
 * @author Hayden McAfee (hayden@outlook.com)
 * @date 2021-01-14
 * @copyright Copyright (c) 2021 Hayden McAfee
 */

#include "FtlServer.h"

#include "ConnectionCreators/ConnectionCreator.h"
#include "ConnectionListeners/ConnectionListener.h"
#include "FtlControlConnection.h"
#include "FtlStream.h"

#pragma region Constructor/Destructor
FtlServer::FtlServer(
    std::unique_ptr<ConnectionListener> ingestControlListener,
    std::unique_ptr<ConnectionCreator> mediaConnectionCreator,
    RequestKeyCallback onRequestKey,
    StreamStartedCallback onStreamStarted,
    StreamEndedCallback onStreamEnded,
    RtpPacketCallback onRtpPacket,
    uint16_t minMediaPort = DEFAULT_MEDIA_MIN_PORT,
    uint16_t maxMediaPort = DEFAULT_MEDIA_MAX_PORT)
:
    ingestControlListener(std::move(ingestControlListener)),
    mediaConnectionCreator(std::move(mediaConnectionCreator)),
    onRequestKey(onRequestKey),
    onStreamStarted(onStreamStarted),
    onStreamEnded(onStreamEnded),
    onRtpPacket(onRtpPacket),
    minMediaPort(minMediaPort),
    maxMediaPort(maxMediaPort)
{
    this->ingestControlListener->SetOnNewConnection(
        std::bind(&FtlServer::onNewControlConnection, this, std::placeholders::_1));
}
#pragma endregion Constructor/Destructor

#pragma region Public functions
void FtlServer::StartAsync()
{
    // Start listening for new ingest connections
    std::promise<void> listenThreadReadyPromise;
    std::future<void> listenThreadReadyFuture = listenThreadReadyPromise.get_future();
    listenThread = 
        std::thread(&FtlServer::ingestThreadBody, this, std::move(listenThreadReadyPromise));
    listenThread.detach();
    listenThreadReadyFuture.get();
}

void FtlServer::Stop()
{

}
#pragma endregion Public functions

#pragma region Private functions
void FtlServer::ingestThreadBody(std::promise<void>&& readyPromise)
{
    ingestControlListener->Listen(std::move(readyPromise));
}

Result<uint16_t> FtlServer::reserveMediaPort()
{
    // TODO: Locking
    for (uint16_t i = minMediaPort; i <= maxMediaPort; ++i)
    {
        if (usedMediaPorts.count(i) <= 0)
        {
            usedMediaPorts.insert(i);
            return Result<uint16_t>::Success(i);
        }
    }
    return Result<uint16_t>::Error("Could not find an available port.");
}

void FtlServer::onNewControlConnection(std::unique_ptr<ConnectionTransport> connection)
{
    // TODO: locking
    auto ingestControlConnection = std::make_unique<FtlControlConnection>(
        std::move(connection),
        onRequestKey,
        std::bind(&FtlServer::onControlStartMediaPort, this),
        std::bind(&FtlServer::onControlConnectionClosed, this));
    auto ingestControlConnectionPtr = ingestControlConnection.get();
    pendingControlConnections.insert_or_assign(ingestControlConnection.get(),
        std::move(ingestControlConnection));
    ingestControlConnectionPtr->StartAsync();
}

Result<uint16_t> FtlServer::onControlStartMediaPort(FtlControlConnection& controlConnection,
    ftl_channel_id_t channelId, FtlStream::MediaMetadata mediaMetadata,
    sockaddr_in targetAddr)
{
    // TODO: locking
    // Locate the control connection in our pending store and pull it out
    if (pendingControlConnections.count(&controlConnection) <= 0)
    {
        throw std::runtime_error("Unknown control connection requested a media port assignment");
    }
    std::unique_ptr<FtlControlConnection> control = 
        std::move(pendingControlConnections[&controlConnection]);
    pendingControlConnections.erase(&controlConnection);

    // Attempt to find a free media port to use
    Result<uint16_t> portResult = reserveMediaPort();
    if (portResult.IsError)
    {
        return portResult;
    }
    uint16_t mediaPort = portResult.Value;

    // Try to start the stream and get a stream ID
    Result<ftl_stream_id_t> streamIdResult = onStreamStarted(channelId);
    if (streamIdResult.IsError)
    {
        return Result<uint16_t>::Error(streamIdResult.ErrorMessage);
    }
    ftl_stream_id_t streamId = streamIdResult.Value;

    // Start a new media connection transport on that port
    std::unique_ptr<ConnectionTransport> mediaTransport = 
        mediaConnectionCreator->CreateConnection(mediaPort, targetAddr);

    // Fire up a new FtlStream and hand over our control connection
    auto stream = std::make_unique<FtlStream>(
        std::move(control), std::move(mediaTransport), mediaMetadata, streamId,
        std::bind(&FtlServer::onStreamClosed, this));
    Result<void> streamStartResult = stream->StartAsync();
    if (streamStartResult.IsError)
    {
        return Result<uint16_t>::Error(streamStartResult.ErrorMessage);
    }
    activeStreams.insert_or_assign(stream.get(), std::move(stream));

    // Pass the media port back to the control connection
    return Result<uint16_t>::Success(mediaPort);
}

void FtlServer::onControlConnectionClosed(FtlControlConnection& controlConnection)
{
    // TODO: locking
    // We should only receive this event if the stream is still pending.
    if (pendingControlConnections.count(&controlConnection) <= 0)
    {
        throw std::runtime_error("Unknown control connection closed.");
    }
    // Just remove the control connection - the stream hasn't started yet, so we don't
    // need to take care of anything else.
    pendingControlConnections.erase(&controlConnection);
}

void FtlServer::onStreamClosed(FtlStream& stream)
{
    // TODO: locking
    if (activeStreams.count(&stream) <= 0)
    {
        throw std::runtime_error("Unknown FTL stream closed.");
    }
    std::unique_ptr<FtlStream> ftlStream = std::move(activeStreams[&stream]);
    activeStreams.erase(&stream);

    onStreamEnded(ftlStream->GetChannelId(), ftlStream->GetStreamId());
}
#pragma endregion Private functions