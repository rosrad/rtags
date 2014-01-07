#ifndef NetworkThread_h
#define NetworkThread_h

/* This file is part of RTags.

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include <rct/Thread.h>
#include <rct/Set.h>
#include <mutex>
#include <condition_variable>

class SocketServer;
class Connection;
class Message;
class NetworkThread : public Thread
{
public:
    NetworkThread();
    void addSocketServer(SocketServer *server);
    virtual void run();
private:
    void onNewConnection(SocketServer *conn);
    void onNewMessage(Message *message, Connection *conn);
    void onConnectionDisconnected(Connection *o);

    bool mStarted;
    Set<SocketServer*> mServers;
    Set<Connection*> mPending;
    std::mutex mMutex;
    std::condition_variable mCond;
};




#endif
