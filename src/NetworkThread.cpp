#include "NetworkThread.h"
#include <rct/Connection.h>
#include <rct/SocketServer.h>

static const bool debugMulti = getenv("RDM_DEBUG_MULTI");

NetworkThread::NetworkThread()
    : mStarted(false)
{
}

void NetworkThread::addSocketServer(SocketServer *server)
{
    assert(!mStarted);
    mServers.insert(server);
}

void NetworkThread::run()
{
    assert(!mStarted);
    {
        std::shared_ptr<EventLoop> eventLoop(new EventLoop);
        for (auto server : mServers) {
            server->newConnection().connect(std::bind(&NetworkThread::onNewConnection, this, std::placeholders::_1));
        }
    }
}

void NetworkThread::onNewConnection(SocketServer *server)
{
    while (true) {
        SocketClient::SharedPtr client = server->nextConnection();
        if (!client)
            break;
        Connection *conn = new Connection(client);
        conn->newMessage().connect(std::bind(&NetworkThread::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
        conn->disconnected().connect(std::bind(&NetworkThread::onConnectionDisconnected, this, std::placeholders::_1));

        if (debugMulti) {
            String ip;
            uint16_t port;
            if (conn->client()->peer(&ip, &port))
                error() << "Got connection from" << String::format<64>("%s:%d", ip.constData(), port);
        }
    }
}
void NetworkThread::onNewMessage(Message *message, Connection *conn)
{

}

void NetworkThread::onConnectionDisconnected(Connection *o)
{

}
