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

#include "Server.h"

#include "CompletionThread.h"
#include "CompileMessage.h"
#include "LogOutputMessage.h"
#include "CursorInfoJob.h"
#include "DependenciesJob.h"
#include "VisitFileResponseMessage.h"
#include "JobAnnouncementMessage.h"
#include "ProxyJobAnnouncementMessage.h"
#include "ExitMessage.h"
#include "ClientMessage.h"
#include "ClientConnectedMessage.h"
#include "PreprocessJob.h"
#include "Filter.h"
#include "FindFileJob.h"
#include "FindSymbolsJob.h"
#include "FollowLocationJob.h"
#include "IndexerJob.h"
#include "Source.h"
#include "Unit.h"
#include "DumpThread.h"
#if defined(HAVE_CXCOMPILATIONDATABASE)
#  include <clang-c/CXCompilationDatabase.h>
#endif
#include "ListSymbolsJob.h"
#include "LogObject.h"
#include "Match.h"
#include "Preprocessor.h"
#include "Project.h"
#include "QueryMessage.h"
#include "VisitFileMessage.h"
#include "IndexerMessage.h"
#include "JobRequestMessage.h"
#include "JobResponseMessage.h"
#include "RTags.h"
#include "ReferencesJob.h"
#include "StatusJob.h"
#include <clang-c/Index.h>
#include <rct/Connection.h>
#include <rct/EventLoop.h>
#include <rct/SocketClient.h>
#include <rct/Log.h>
#include <rct/Message.h>
#include <rct/Messages.h>
#include <rct/Path.h>
#include <rct/Process.h>
#include <rct/Rct.h>
#include <rct/RegExp.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <limits>

class HttpLogObject : public LogOutput
{
public:
    HttpLogObject(int logLevel, const SocketClient::SharedPtr &socket)
        : LogOutput(logLevel), mSocket(socket)
    {}

    virtual bool testLog(int level) const
    {
        return level == logLevel();
    }
    virtual void log(const char *msg, int len)
    {
        if (!EventLoop::isMainThread()) {
            String message(msg, len);
            SocketClient::WeakPtr weak = mSocket;

            EventLoop::eventLoop()->callLater(std::bind([message,weak,this] {
                        // ### I don't understand why I need to capture this
                        // ### here (especially since this potentially could
                        // ### have been destroyed but it doesn't compile
                        // ### otherwise.
                        if (SocketClient::SharedPtr socket = weak.lock()) {
                            HttpLogObject::send(message.constData(), message.size(), socket);
                        }
                    }));
        } else {
            send(msg, len, mSocket);
        }
    }
    static void send(const char *msg, int len, const SocketClient::SharedPtr &socket)
    {
        static const unsigned char *header = reinterpret_cast<const unsigned char*>("data:");
        static const unsigned char *crlf = reinterpret_cast<const unsigned char*>("\r\n");
        socket->write(header, 5);
        socket->write(reinterpret_cast<const unsigned char *>(msg), len);
        socket->write(crlf, 2);
    }
private:
    SocketClient::SharedPtr mSocket;
};

static const bool debugMulti = getenv("RDM_DEBUG_MULTI");

Server *Server::sInstance = 0;
Server::Server()
    : mVerbose(false), mConnectToServerFailures(0), mThreadPool(0),
      mServerConnection(0), mCompletionThread(0), mFirstRemote(0), mLastRemote(0),
      mAnnounced(false), mWorkPending(false), mExitCode(0)
{
    Messages::registerMessage<JobRequestMessage>();
    Messages::registerMessage<JobResponseMessage>();
    Messages::registerMessage<ClientConnectedMessage>();
    Messages::registerMessage<ExitMessage>();

    assert(!sInstance);
    sInstance = this;

    mUnloadTimer.timeout().connect(std::bind(&Server::onUnload, this));
    mConnectToServerTimer.timeout().connect(std::bind(&Server::connectToServer, this));
    mRescheduleTimer.timeout().connect(std::bind(&Server::onReschedule, this));
}

Server::~Server()
{
    if (mCompletionThread) {
        mCompletionThread->stop();
        mCompletionThread->join();
        delete mCompletionThread;
        mCompletionThread = 0;
    }

    Rct::LinkedList::deleteAll(mFirstRemote);

    for (const auto &job : mLocalJobs) {
        job.first->kill();
    }

    clear();
    assert(sInstance == this);
    sInstance = 0;
    Messages::cleanup();
}

void Server::clear()
{
    stopServers();
    delete mThreadPool; // wait first?
    mThreadPool = 0;
}

bool Server::init(const Options &options)
{
    RTags::initMessages();

    mOptions = options;
    Path clangPath = Path::resolved(CLANG_INCLUDEPATH);
    mOptions.includePaths.append(clangPath);
#ifdef OS_Darwin
    if (clangPath.exists()) {
        Path cppClangPath = clangPath + "../../../c++/v1/";
        cppClangPath.resolve();
        if (cppClangPath.isDir()) {
            mOptions.includePaths.append(cppClangPath);
        } else {
            cppClangPath = clangPath + "../../../../include/c++/v1/";
            cppClangPath.resolve();
            if (cppClangPath.isDir()) {
                mOptions.includePaths.append(cppClangPath);
            }
        }
        // this seems to be the only way we get things like cstdint
    }
#endif

    if (options.options & UnlimitedErrors)
        mOptions.defaultArguments << "-ferror-limit=0";
    if (options.options & Wall)
        mOptions.defaultArguments << "-Wall";
    if (options.options & SpellChecking)
        mOptions.defaultArguments << "-fspell-checking";
    if (!(options.options & NoNoUnknownWarningsOption))
        mOptions.defaultArguments.append("-Wno-unknown-warning-option");
    Log l(Error);
    l << "Running with" << mOptions.jobCount << "jobs, using args:"
      << String::join(mOptions.defaultArguments, ' ') << '\n';
    if (mOptions.tcpPort || mOptions.multicastPort || mOptions.httpPort) {
        if (mOptions.tcpPort)
            l << "tcp-port:" << mOptions.tcpPort;
        if (mOptions.multicastPort)
            l << "multicast-port:" << mOptions.multicastPort;
        if (mOptions.httpPort)
            l << "http-port:" << mOptions.httpPort;
        l << '\n';
    }
    l << "includepaths" << String::join(mOptions.includePaths, ' ');

    if (mOptions.options & ClearProjects) {
        clearProjects();
    }

    for (int i=0; i<10; ++i) {
        mUnixServer.reset(new SocketServer);
        if (mUnixServer->listen(mOptions.socketFile)) {
            break;
        }
        mUnixServer.reset();
        if (!i) {
            enum { Timeout = 1000 };
            Connection connection;
            if (connection.connectUnix(mOptions.socketFile, Timeout)) {
                connection.send(QueryMessage(QueryMessage::Shutdown));
                connection.disconnected().connect(std::bind([](){ EventLoop::eventLoop()->quit(); }));
                connection.finished().connect(std::bind([](){ EventLoop::eventLoop()->quit(); }));
                EventLoop::eventLoop()->exec(Timeout);
            }
        } else {
            sleep(1);
        }
        Path::rm(mOptions.socketFile);
    }
    if (!mUnixServer) {
        error("Unable to listen on %s", mOptions.socketFile.constData());
        return false;
    }

    restoreFileIds();
    mUnixServer->newConnection().connect(std::bind(&Server::onNewConnection, this, std::placeholders::_1));
    reloadProjects();
    if (!(mOptions.options & NoStartupCurrentProject)) {
        Path current = Path(mOptions.dataDir + ".currentProject").readAll(1024);
        if (current.size() > 1) {
            current.chop(1);
            const auto project = mProjects.value(current);
            if (!project) {
                error() << "Can't restore project" << current;
                unlink((mOptions.dataDir + ".currentProject").constData());
            } else {
                setCurrentProject(project);
            }
        }
    }

    if (!mOptions.multicastAddress.isEmpty()) {
        mMulticastSocket.reset(new SocketClient);
        if (!mMulticastSocket->bind(mOptions.multicastPort)) {
            error() << "Can't bind to multicast port" << mOptions.multicastPort;
        }
        if (!mMulticastSocket->addMembership(mOptions.multicastAddress)) {
            error() << "Can't add membership" << mOptions.multicastAddress;
            return false;
        }
        mMulticastSocket->setMulticastLoop(false);
        if (mOptions.multicastTTL)
            mMulticastSocket->setMulticastTTL(mOptions.multicastTTL);
        mMulticastSocket->readyReadFrom().connect(std::bind(&Server::onMulticastReadyRead, this,
                                                            std::placeholders::_1,
                                                            std::placeholders::_2,
                                                            std::placeholders::_3,
                                                            std::placeholders::_4));
    }

    if (mOptions.tcpPort) {
        mTcpServer.reset(new SocketServer);
        if (!mTcpServer->listen(mOptions.tcpPort)) {
            error() << "Unable to listen on port" << mOptions.tcpPort;
            return false;
        }

        mTcpServer->newConnection().connect(std::bind(&Server::onNewConnection, this, std::placeholders::_1));
    }
    if ((mOptions.options & (NoJobServer|ForcePreprocessing)) != NoJobServer) {
        mThreadPool = new ThreadPool(std::max(1, mOptions.jobCount),
                                     Thread::Normal,
                                     mOptions.threadStackSize);
    }

    if (mOptions.httpPort) {
        mHttpServer.reset(new SocketServer);
        if (!mHttpServer->listen(mOptions.httpPort)) {
            error() << "Unable to listen on http-port:" << mOptions.httpPort;
            // return false;
            mHttpServer.reset();
        } else {
            mHttpServer->newConnection().connect(std::bind([this](){
                        while (SocketClient::SharedPtr client = mHttpServer->nextConnection()) {
                            mHttpClients[client] = 0;
                            client->disconnected().connect(std::bind([this, client] { mHttpClients.remove(client); }));
                            client->readyRead().connect(std::bind(&Server::onHttpClientReadyRead, this, std::placeholders::_1));
                        }
                    }));
        }

    }

    if (!(mOptions.options & JobServer))
        connectToServer();

    return true;
}

std::shared_ptr<Project> Server::addProject(const Path &path) // lock always held
{
    std::shared_ptr<Project> &project = mProjects[path];
    if (!project) {
        project.reset(new Project(path));
        return project;
    }
    return std::shared_ptr<Project>();
}

int Server::reloadProjects()
{
    mProjects.clear(); // ### could keep the ones that persist somehow
    List<Path> projects = mOptions.dataDir.files(Path::File);
    const Path home = Path::home();
    for (int i=0; i<projects.size(); ++i) {
        Path file = projects.at(i);
        Path p = file.mid(mOptions.dataDir.size());
        RTags::decodePath(p);
        if (p.isDir()) {
            bool remove = false;
            if (FILE *f = fopen(file.constData(), "r")) {
                Deserializer in(f);
                int version;
                in >> version;

                if (version == RTags::DatabaseVersion) {
                    int fs;
                    in >> fs;
                    if (fs != Rct::fileSize(f)) {
                        error("%s seems to be corrupted, refusing to restore. Removing.",
                              file.constData());
                        remove = true;
                    } else {
                        addProject(p);
                    }
                } else {
                    remove = true;
                    error() << file << "has wrong format. Got" << version << "expected" << RTags::DatabaseVersion << "Removing";
                }
                fclose(f);
            }
            if (remove) {
                Path::rm(file);
            }
        }
    }
    return mProjects.size();
}

void Server::onNewConnection(SocketServer *server)
{
    while (true) {
        SocketClient::SharedPtr client = server->nextConnection();
        if (!client)
            break;
        Connection *conn = new Connection(client);
        conn->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
        conn->disconnected().connect(std::bind(&Server::onConnectionDisconnected, this, std::placeholders::_1));

        if (debugMulti && !conn->client()->peerString().isEmpty()) {
            error() << "Got connection from" << Rct::addrLookup(conn->client()->peerName());
        }
    }
}

void Server::onConnectionDisconnected(Connection *o)
{
    o->disconnected().disconnect();
    if (mClients.remove(o)) {
        error() << "Client disappeared" << Rct::addrLookup(o->client()->peerName());
    }
    EventLoop::deleteLater(o);
    mPendingJobRequests.remove(o);
}

void Server::onNewMessage(Message *message, Connection *connection)
{
    if (mOptions.unloadTimer)
        mUnloadTimer.restart(mOptions.unloadTimer * 1000 * 60, Timer::SingleShot);

    RTagsMessage *m = static_cast<RTagsMessage*>(message);

    switch (message->messageId()) {
    case CompileMessage::MessageId:
        handleCompileMessage(static_cast<CompileMessage&>(*m), connection);
        break;
    case QueryMessage::MessageId:
        handleQueryMessage(static_cast<const QueryMessage&>(*m), connection);
        break;
    case IndexerMessage::MessageId:
        handleIndexerMessage(static_cast<const IndexerMessage&>(*m), connection);
        break;
    case LogOutputMessage::MessageId:
        error() << m->raw();
        handleLogOutputMessage(static_cast<const LogOutputMessage&>(*m), connection);
        break;
    case ExitMessage::MessageId:
        handleExitMessage(static_cast<const ExitMessage&>(*m));
        break;
    case VisitFileMessage::MessageId:
        handleVisitFileMessage(static_cast<const VisitFileMessage&>(*m), connection);
        break;
    case ResponseMessage::MessageId:
    case FinishMessage::MessageId:
    case VisitFileResponseMessage::MessageId:
        error() << getpid() << "Unexpected message" << static_cast<int>(message->messageId());
        // assert(0);
        connection->finish(1);
        break;
    case JobRequestMessage::MessageId:
        handleJobRequestMessage(static_cast<const JobRequestMessage&>(*m), connection);
        break;
    case ClientConnectedMessage::MessageId:
        handleClientConnectedMessage(static_cast<const ClientConnectedMessage&>(*m));
        break;
    case JobAnnouncementMessage::MessageId:
        handleJobAnnouncementMessage(static_cast<const JobAnnouncementMessage&>(*m), connection);
        break;
    case JobResponseMessage::MessageId:
        handleJobResponseMessage(static_cast<const JobResponseMessage&>(*m), connection);
        break;
    case ProxyJobAnnouncementMessage::MessageId:
        handleProxyJobAnnouncementMessage(static_cast<const ProxyJobAnnouncementMessage&>(*m), connection);
        break;
    case ClientMessage::MessageId:
        handleClientMessage(static_cast<const ClientMessage&>(*m), connection);
        break;
    default:
        error("Unknown message: %d", message->messageId());
        connection->finish(1);
        break;
    }
    if (mOptions.options & NoFileManagerWatch) {
        std::shared_ptr<Project> project = currentProject();
        if (project && project->fileManager && (Rct::monoMs() - project->fileManager->lastReloadTime()) > 60000)
            project->fileManager->reload(FileManager::Asynchronous);
    }
}

bool Server::index(const String &arguments, const Path &pwd, const Path &projectRootOverride, bool escape)
{
    Path unresolvedPath;
    unsigned int flags = Source::None;
    if (escape)
        flags |= Source::Escape;
    List<Path> unresolvedPaths;
    List<Source> sources = Source::parse(arguments, pwd, flags, &unresolvedPaths);
    bool ret = false;
    int idx = 0;
    for (Source &source : sources) {
        const Path path = source.sourceFile();

        std::shared_ptr<Project> current = currentProject();
        Path root;
        const Path unresolvedPath = unresolvedPaths.at(idx++);
        if (current && (current->match(unresolvedPath) || (path != unresolvedPath && current->match(path)))) {
            root = current->path();
        } else {
            for (const auto &proj : mProjects) {
                if (proj.second->match(unresolvedPath) || (path != unresolvedPath && proj.second->match(path))) {
                    root = proj.first;
                    break;
                }
            }
        }

        if (root.isEmpty()) {
            root = projectRootOverride;
            if (root.isEmpty()) {
                root = RTags::findProjectRoot(unresolvedPath, RTags::SourceRoot);
                if (root.isEmpty() && path != unresolvedPath)
                    root = RTags::findProjectRoot(path, RTags::SourceRoot);
            }
        }

        if (shouldIndex(source, root)) {
            preprocess(std::move(source), std::move(root), IndexerJob::Compile);
            ret = true;
        }
    }
    return ret;
}

void Server::preprocess(Source &&source, Path &&srcRoot, uint32_t flags)
{
    std::shared_ptr<Project> project = mProjects.value(srcRoot);
    if (!project) {
        project = addProject(srcRoot);
        assert(project);
    }
    project->load();

    WorkScope scope;
    if (!(mOptions.options & ForcePreprocessing) && !hasServer()) {
        if (debugMulti)
            error() << "Not preprocessing" << source.sourceFile() << "since we're not on the farm";
        std::shared_ptr<Unit> unit(new Unit);
        unit->flags = flags;
        unit->time = Rct::currentTimeMs();
        unit->preprocessDuration = 0;
        unit->source = std::move(source);
        unit->sourceFile = source.sourceFile();
        index(unit, project);
    } else {
        if (mOptions.options & CompressionAlways)
            flags |= IndexerJob::PreprocessCompressed;
        std::shared_ptr<PreprocessJob> job(new PreprocessJob(std::move(source), project, flags));
        mPendingPreprocessJobs.append(job);
    }
}

void Server::handleCompileMessage(CompileMessage &message, Connection *conn)
{
#if defined(HAVE_CXCOMPILATIONDATABASE) && CLANG_VERSION_MINOR >= 3
    const Path path = message.compilationDatabaseDir();
    if (!path.isEmpty()) {
        CXCompilationDatabase_Error err;
        CXCompilationDatabase db = clang_CompilationDatabase_fromDirectory(path.constData(), &err);
        if (err != CXCompilationDatabase_NoError) {
            conn->write("Can't load compilation database");
            conn->finish();
            return;
        }
        CXCompileCommands cmds = clang_CompilationDatabase_getAllCompileCommands(db);
        const unsigned int sz = clang_CompileCommands_getSize(cmds);
        for (unsigned int i = 0; i < sz; ++i) {
            CXCompileCommand cmd = clang_CompileCommands_getCommand(cmds, i);
            String args;
            CXString str = clang_CompileCommand_getDirectory(cmd);
            Path dir = clang_getCString(str);
            clang_disposeString(str);
            const unsigned int num = clang_CompileCommand_getNumArgs(cmd);
            for (unsigned int j = 0; j < num; ++j) {
                str = clang_CompileCommand_getArg(cmd, j);
                args += clang_getCString(str);
                clang_disposeString(str);
                if (j < num - 1)
                    args += " ";
            }

            index(args, dir, message.projectRoot(), message.escape());
        }
        clang_CompileCommands_dispose(cmds);
        clang_CompilationDatabase_dispose(db);
        conn->write("Compilation database loaded");
        conn->finish();
        return;
    }
#endif
    const bool ret = index(message.arguments(), message.workingDirectory(),
                           message.projectRoot(), message.escape());
    conn->finish(ret ? 0 : 1);
}

void Server::handleExitMessage(const ExitMessage &message)
{
    mExitCode = message.exitCode();
    if (mServerConnection && message.forward()) {
        mServerConnection->send(message);
    } else if (!mClients.isEmpty()) {
        const ExitMessage msg(mExitCode, false);
        for (const auto &client : mClients) {
            if (debugMulti) {
                error() << "Telling" << Rct::addrLookup(client->client()->peerName())
                        << "to shut down with status code" << message.exitCode();
            }

            client->send(msg);
        }
    } else {
        EventLoop::eventLoop()->quit();
        return;
    }

    EventLoop::eventLoop()->registerTimer(std::bind(&EventLoop::quit, EventLoop::eventLoop()),
                                          1000, Timer::SingleShot);
}

void Server::handleLogOutputMessage(const LogOutputMessage &message, Connection *conn)
{
    new LogObject(conn, message.level());
}

void Server::handleIndexerMessage(const IndexerMessage &message, Connection *conn)
{
    WorkScope scope;
    std::shared_ptr<IndexData> indexData = message.data();
    // error() << "Got indexer message" << message.project() << Location::path(indexData->fileId);
    assert(indexData);
    auto it = mProcessingJobs.find(indexData->jobId);
    if (debugMulti)
        error() << "got indexer message for job" << Location::path(indexData->fileId()) << indexData->jobId
                << "from" << (conn->client()->peerString().isEmpty()
                              ? String("ourselves")
                              : Rct::addrLookup(conn->client()->peerName()).constData());
    if (it != mProcessingJobs.end()) {
        std::shared_ptr<IndexerJob> job = it->second;
        assert(job);
        mProcessingJobs.erase(it);
        assert(!(job->unit->flags & IndexerJob::FromRemote));

        const String ip = conn->client()->peerName();
        if (!ip.isEmpty())
            indexData->message << String::format<64>(" from %s", Rct::addrLookup(ip).constData());

        const IndexerJob::Flag runningFlag = (ip.isEmpty() ? IndexerJob::RunningLocal : IndexerJob::Remote);
        job->unit->flags &= ~runningFlag;

        // we only care about the first job that returns
        if (!(job->unit->flags & (IndexerJob::CompleteLocal|IndexerJob::CompleteRemote))) {
            if (!(job->unit->flags & IndexerJob::Aborted))
                job->unit->flags |= (ip.isEmpty() ? IndexerJob::CompleteLocal : IndexerJob::CompleteRemote);
            std::shared_ptr<Project> project = mProjects.value(message.project());
            if (!project) {
                error() << "Can't find project root for this IndexerMessage" << message.project() << Location::path(indexData->fileId());
            } else {
                project->onJobFinished(indexData, job);
            }
        }
    } else {
        // job already processed
        if (debugMulti)
            error() << "already got a response for" << indexData->jobId;
    }
    conn->finish();
}

void Server::handleQueryMessage(const QueryMessage &message, Connection *conn)
{
    if (!(message.flags() & QueryMessage::SilentQuery))
        error() << message.raw();
    conn->setSilent(message.flags() & QueryMessage::Silent);

    switch (message.type()) {
    case QueryMessage::Invalid:
        assert(0);
        break;
    case QueryMessage::SyncProject:
        syncProject(message, conn);
        break;
    case QueryMessage::Sources:
        sources(message, conn);
        break;
    case QueryMessage::DumpCompletions:
        dumpCompletions(message, conn);
        break;
    case QueryMessage::SendDiagnostics:
        sendDiagnostics(message, conn);
        break;
    case QueryMessage::CodeCompleteAt:
    case QueryMessage::PrepareCodeCompleteAt:
        codeCompleteAt(message, conn);
        break;
    case QueryMessage::SuspendFile:
        suspendFile(message, conn);
        break;
    case QueryMessage::IsIndexing:
        isIndexing(message, conn);
        break;
    case QueryMessage::RemoveFile:
        removeFile(message, conn);
        break;
    case QueryMessage::JobCount:
        jobCount(message, conn);
        break;
    case QueryMessage::FixIts:
        fixIts(message, conn);
        break;
    case QueryMessage::FindFile:
        findFile(message, conn);
        break;
    case QueryMessage::DumpFile:
        dumpFile(message, conn);
        break;
    case QueryMessage::Dependencies:
        dependencies(message, conn);
        break;
    case QueryMessage::DeleteProject:
        removeProject(message, conn);
        break;
    case QueryMessage::UnloadProject:
        removeProject(message, conn);
        break;
    case QueryMessage::ReloadProjects:
        reloadProjects(message, conn);
        break;
    case QueryMessage::Project:
        project(message, conn);
        break;
    case QueryMessage::Reindex: {
        reindex(message, conn);
        break; }
    case QueryMessage::ClearProjects:
        clearProjects(message, conn);
        break;
    case QueryMessage::CursorInfo:
        cursorInfo(message, conn);
        break;
    case QueryMessage::Shutdown:
        shutdown(message, conn);
        break;
    case QueryMessage::FollowLocation:
        followLocation(message, conn);
        break;
    case QueryMessage::ReferencesLocation:
        referencesForLocation(message, conn);
        break;
    case QueryMessage::ReferencesName:
        referencesForName(message, conn);
        break;
    case QueryMessage::ListSymbols:
        listSymbols(message, conn);
        break;
    case QueryMessage::FindSymbols:
        findSymbols(message, conn);
        break;
    case QueryMessage::Status:
        status(message, conn);
        break;
    case QueryMessage::IsIndexed:
        isIndexed(message, conn);
        break;
    case QueryMessage::HasFileManager:
        hasFileManager(message, conn);
        break;
    case QueryMessage::PreprocessFile:
        preprocessFile(message, conn);
        break;
    case QueryMessage::ReloadFileManager:
        reloadFileManager(message, conn);
        break;
    }
}

void Server::followLocation(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->write("Not indexed");
        conn->finish(1);
        return;
    }
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project) {
        error("No project");
        conn->finish(1);
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish(2);
        return;
    }

    int ret;
    {
        FollowLocationJob job(loc, query, project);
        ret = job.run(conn);
        if (!ret) {
            conn->finish(ret);
            return;
        }
    }

    /* We will try with another project under the following circumstances:

       - We didn't find anything with the current project
       - The path in question (likely a header) does not start with the current
       project's path (there's room for mistakes here with symlinks).
       - The file in question does start with another project's path
       - The other project is loaded (we will start loading it if it's not)
    */

    const Path path = loc.path();
    if (!path.startsWith(project->path())) {
        for (const auto &proj : mProjects) {
            if (proj.second != project) {
                Path paths[] = { proj.first, proj.first };
                paths[1].resolve();
                for (const Path &projectPath : paths) {
                    if (path.startsWith(projectPath) && !proj.second->load(Project::FileManager_Asynchronous)) {
                        FollowLocationJob job(loc, query, proj.second);
                        ret = job.run(conn);
                        if (!ret) {
                            conn->finish(ret);
                            return;
                        }
                    }
                }
            }
        }
    }
    conn->finish(ret);
}

void Server::isIndexing(const QueryMessage &, Connection *conn)
{
    for (const auto &it : mProjects) {
        if (it.second->isIndexing()) {
            conn->write("1");
            conn->finish();
            return;
        }
    }
    conn->write("0");
    conn->finish();
}

void Server::removeFile(const QueryMessage &query, Connection *conn)
{
    // Path path = query.path();
    const Match match = query.match();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project)
        project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    const int count = project->remove(match);
    // error() << count << query.query();
    if (count) {
        conn->write<128>("Removed %d files", count);
    } else {
        conn->write("No matches");
    }
    conn->finish();
}

void Server::findFile(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();
    if (!project || project->state() == Project::Unloaded) {
        error("No project");
        conn->finish();
        return;
    }

    FindFileJob job(query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::dumpFile(const QueryMessage &query, Connection *conn)
{
    const uint32_t fileId = Location::fileId(query.query());
    if (!fileId) {
        conn->write<256>("%s is not indexed", query.query().constData());
        conn->finish();
        return;
    }

    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project || project->state() != Project::Loaded) {
        conn->write<256>("%s is not indexed", query.query().constData());
        conn->finish();
        return;
    }

    const Source source = project->sources(fileId).value(query.buildIndex());
    if (!source.isNull()) {
        conn->disconnected().disconnect();
        // ### this is a hack, but if the connection goes away we can't post
        // ### events to it. We could fix this nicer but I didn't

        DumpThread *dumpThread = new DumpThread(query, source, conn);
        dumpThread->start(Thread::Normal, 8 * 1024 * 1024); // 8MiB stack size
    } else {
        conn->write<256>("%s build: %d not found", query.query().constData(), query.buildIndex());
        conn->finish();
    }
}

void Server::cursorInfo(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->finish();
        return;
    }
    std::shared_ptr<Project> project = projectForQuery(query);

    if (!project) {
        conn->finish();
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
    } else {
        CursorInfoJob job(loc, query, project);
        const int ret = job.run(conn);
        conn->finish(ret);
    }
}

void Server::dependencies(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project) {
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    DependenciesJob job(query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::fixIts(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = projectForQuery(query);
    if (project && project->state() == Project::Loaded) {
        String out = project->fixIts(Location::fileId(query.query()));
        if (!out.isEmpty())
            conn->write(out);
    }
    conn->finish();
}

void Server::referencesForLocation(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->write("Not indexed");
        conn->finish();
        return;
    }
    std::shared_ptr<Project> project = projectForQuery(query);

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    ReferencesJob job(loc, query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::referencesForName(const QueryMessage& query, Connection *conn)
{
    const String name = query.query();

    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    ReferencesJob job(name, query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::findSymbols(const QueryMessage &query, Connection *conn)
{
    const String partial = query.query();

    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    FindSymbolsJob job(query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::listSymbols(const QueryMessage &query, Connection *conn)
{
    const String partial = query.query();

    std::shared_ptr<Project> project = currentProject();
    if (!project) {
        error("No project");
        conn->finish();
        return;
    }

    ListSymbolsJob job(query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::status(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    conn->client()->setWriteMode(SocketClient::Synchronous);

    StatusJob job(query, project);
    const int ret = job.run(conn);
    conn->finish(ret);
}

void Server::isIndexed(const QueryMessage &query, Connection *conn)
{
    String ret = "unknown";
    const Match match = query.match();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (project) {
        bool indexed = false;
        if (project->match(match, &indexed))
            ret = indexed ? "indexed" : "managed";
    }

    if (!(query.flags() & QueryMessage::SilentQuery))
        error("=> %s", ret.constData());
    conn->write(ret);
    conn->finish();
}

void Server::reloadFileManager(const QueryMessage &, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();
    if (project) {
        conn->write<512>("Reloading files for %s", project->path().constData());
        conn->finish();
        project->fileManager->reload(FileManager::Asynchronous);
    } else {
        conn->write("No current project");
        conn->finish();
    }
}

void Server::hasFileManager(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (project && project->fileManager && (project->fileManager->contains(path) || project->match(query.match()))) {
        if (!(query.flags() & QueryMessage::SilentQuery))
            error("=> 1");
        conn->write("1");
    } else {
        if (!(query.flags() & QueryMessage::SilentQuery))
            error("=> 0");
        conn->write("0");
    }
    conn->finish();
}

void Server::preprocessFile(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project) {
        conn->write("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    const uint32_t fileId = Location::fileId(path);
    const Source source = project->sources(fileId).value(query.buildIndex());
    if (!source.isValid()) {
        conn->write<256>("%s build: %d not found", query.query().constData(), query.buildIndex());
    } else {
        Preprocessor pre(source, conn);
        pre.preprocess();
    }
    conn->finish();
}

void Server::clearProjects()
{
    for (const auto &it : mProjects)
        it.second->unload();
    Rct::removeDirectory(mOptions.dataDir);
    setCurrentProject(std::shared_ptr<Project>());
    mProjects.clear();
}

void Server::reindex(const QueryMessage &query, Connection *conn)
{
    Match match = query.match();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project) {
        project = currentProject();
        if (!project) {
            error("No project");
            conn->finish();
            return;
        } else if (project->state() != Project::Loaded) {
            conn->write("Project loading");
            conn->finish();
            return;
        }
    }

    const int count = project->reindex(match);
    // error() << count << query.query();
    if (count) {
        conn->write<128>("Dirtied %d files", count);
    } else {
        conn->write("No matches");
    }
    conn->finish();
}

bool Server::shouldIndex(const Source &source, const Path &srcRoot) const
{
    if (srcRoot.isEmpty()) {
        warning() << "Shouldn't index" << source.sourceFile() << "because of missing srcRoot";
        return false;
    }
    assert(source.isIndexable());
    if (mOptions.ignoredCompilers.contains(source.compiler())) {
        warning() << "Shouldn't index" << source.sourceFile() << "because of ignored compiler";
        return false;
    }

    const Path sourceFile = source.sourceFile();

    if (Filter::filter(sourceFile, mOptions.excludeFilters) == Filter::Filtered) {
        warning() << "Shouldn't index" << source.sourceFile() << "because of exclude filter";
        return false;
    }

    std::shared_ptr<Project> project = mProjects.value(srcRoot);
    if (project && project->hasSource(source)) {
        warning() << "Shouldn't index" << source.sourceFile() << "because we already have indexed it";
        return false;
    }
    return true;
}

void Server::index(const std::shared_ptr<Unit> &unit, const std::shared_ptr<Project> &project)
{
    warning() << "Indexing" << unit->source << "in" << project->path();
    if (!currentProject())
        setCurrentProject(project);
    assert(project);
    project->index(unit);
}

void Server::setCurrentProject(const std::shared_ptr<Project> &project, unsigned int queryFlags)
{
    std::shared_ptr<Project> old = currentProject();
    if (project != old) {
        if (old && old->fileManager)
            old->fileManager->clearFileSystemWatcher();
        mCurrentProject = project;
        if (project) {
            Path::mkdir(mOptions.dataDir);
            FILE *f = fopen((mOptions.dataDir + ".currentProject").constData(), "w");
            if (f) {
                if (!fwrite(project->path().constData(), project->path().size(), 1, f) || !fwrite("\n", 1, 1, f)) {
                    error() << "error writing to" << (mOptions.dataDir + ".currentProject");
                    fclose(f);
                    unlink((mOptions.dataDir + ".currentProject").constData());
                } else {
                    fclose(f);
                }
            } else {
                error() << "error opening" << (mOptions.dataDir + ".currentProject") << "for write";
            }
            Project::FileManagerMode mode = Project::FileManager_Asynchronous;
            if (queryFlags & QueryMessage::WaitForLoadProject)
                mode = Project::FileManager_Synchronous;
            switch (project->state()) {
            case Project::Loaded:
            case Project::Inited:
                project->fileManager->reload(FileManager::Asynchronous);
                break;
            default:
                break;
            }
            project->load(mode);
        } else {
            Path::rm(mOptions.dataDir + ".currentProject");
        }
    }
}

std::shared_ptr<Project> Server::projectForQuery(const QueryMessage &query)
{
    Match matches[2];
    if (query.flags() & QueryMessage::HasLocation) {
        matches[0] = query.location().path();
    } else {
        matches[0] = query.match();
    }
    matches[1] = query.currentFile();
    std::shared_ptr<Project> cur = currentProject();
    // give current a chance first to avoid switching project when using system headers etc
    for (int i=0; i<2; ++i) {
        const Match &match = matches[i];
        if (cur && cur->match(match))
            return cur;

        for (const auto &it : mProjects) {
            if (it.second != cur && it.second->match(match)) {
                setCurrentProject(it.second, query.flags());
                return it.second;
            }
        }
    }
    return std::shared_ptr<Project>();
}

void Server::removeProject(const QueryMessage &query, Connection *conn)
{
    const bool unload = query.type() == QueryMessage::UnloadProject;

    const Match match = query.match();
    auto it = mProjects.begin();
    bool found = false;
    while (it != mProjects.end()) {
        auto cur = it++;
        if (cur->second->match(match)) {
            found = true;
            if (currentProject() == cur->second) {
                setCurrentProject(std::shared_ptr<Project>());
            }
            cur->second->unload();
            Path path = cur->first;
            conn->write<128>("%s project: %s", unload ? "Unloaded" : "Deleted", path.constData());
            if (!unload) {
                RTags::encodePath(path);
                Path::rm(mOptions.dataDir + path);
                mProjects.erase(cur);
            }
        }
    }
    if (!found) {
        conn->write<128>("No projects matching %s", match.pattern().constData());
    }
    conn->finish();
}

void Server::reloadProjects(const QueryMessage &query, Connection *conn)
{
    const int old = mProjects.size();
    const int cur = reloadProjects();
    conn->write<128>("Changed from %d to %d projects", old, cur);
    conn->finish();
}

void Server::project(const QueryMessage &query, Connection *conn)
{
    if (query.query().isEmpty()) {
        const std::shared_ptr<Project> current = currentProject();
        const char *states[] = { "(unloaded)", "(inited)", "(loading)", "(loaded)", "(syncing)" };
        for (const auto &it : mProjects) {
            conn->write<128>("%s %s%s", it.first.constData(), states[it.second->state()], it.second == current ? " <=" : "");
        }
    } else {
        std::shared_ptr<Project> selected;
        bool error = false;
        const Match match = query.match();
        const auto it = mProjects.find(match.pattern());
        bool ok = false;
        unsigned long long index = query.query().toULongLong(&ok);
        if (it != mProjects.end()) {
            selected = it->second;
        } else {
            for (const auto &pit : mProjects) {
                assert(pit.second);
                if (ok) {
                    if (!index) {
                        selected = pit.second;
                    } else {
                        --index;
                    }
                }
                if (pit.second->match(match)) {
                    if (error) {
                        conn->write(pit.first);
                    } else if (selected) {
                        error = true;
                        conn->write<128>("Multiple matches for %s", match.pattern().constData());
                        conn->write(selected->path());
                        conn->write(pit.first);
                        selected.reset();
                    } else {
                        selected = pit.second;
                    }
                }
            }
        }
        if (selected) {
            if (selected == currentProject()) {
                conn->write<128>("%s is already the active project", selected->path().constData());
            } else {
                setCurrentProject(selected);
                conn->write<128>("Selected project: %s for %s",
                                 selected->path().constData(),
                                 match.pattern().constData());
            }
        } else if (!error) {
            conn->write<128>("No matches for %s", match.pattern().constData());
        }
    }
    conn->finish();
}

void Server::jobCount(const QueryMessage &query, Connection *conn)
{
    WorkScope scope;
    if (query.query().isEmpty()) {
        conn->write<128>("Running with %d jobs", mOptions.jobCount);
    } else {
        const int jobCount = query.query().toLongLong();
        if (jobCount < 0 || jobCount > 100) {
            conn->write<128>("Invalid job count %s (%d)", query.query().constData(), jobCount);
        } else {
            mOptions.jobCount = jobCount;
            if (mThreadPool)
                mThreadPool->setConcurrentJobs(std::max(1, jobCount));
            conn->write<128>("Changed jobs to %d", jobCount);
        }
    }
    conn->finish();
}

void Server::sendDiagnostics(const QueryMessage &query, Connection *conn)
{
    if (testLog(RTags::CompilationErrorXml))
        logDirect(RTags::CompilationErrorXml, query.query());
    conn->finish();
}

void Server::clearProjects(const QueryMessage &query, Connection *conn)
{
    clearProjects();
    conn->write("Cleared projects");
    conn->finish();
}

void Server::shutdown(const QueryMessage &query, Connection *conn)
{
    for (const auto &it : mProjects) {
        if (it.second)
            it.second->unload();
    }
    if (!query.query().isEmpty()) {
        int exitCode;
        Deserializer deserializer(query.query());
        deserializer >> exitCode;
        ExitMessage msg(exitCode, true);
        handleExitMessage(msg);
    } else {
        EventLoop::eventLoop()->quit();
    }
    conn->write("Shutting down");
    conn->finish();
}

void Server::sources(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    const bool flagsOnly = query.flags() & QueryMessage::CompilationFlagsOnly;
    const bool splitLine = query.flags() & QueryMessage::CompilationFlagsSplitLine;
    if (path.isFile()) {
        std::shared_ptr<Project> project = projectForQuery(query);
        if (project) {
            if (project->state() != Project::Loaded) {
                conn->write("Project loading");
            } else {
                const uint32_t fileId = Location::fileId(path);
                if (fileId) {
                    const List<Source> sources = project->sources(fileId);
                    int idx = 0;
                    for (const auto &it : sources) {
                        String out;
                        if (sources.size() > 1)
                            out = String::format<4>("%d: ", idx);
                        if (flagsOnly) {
                            out += String::join(it.toCommandLine(0), splitLine ? '\n' : ' ');
                        } else {
                            out += it.toString();
                        }
                        conn->write(out);
                    }
                }
            }
            conn->finish();
            return;
        }
    }

    if (std::shared_ptr<Project> project = currentProject()) {
        const Match match = query.match();
        if (project->state() != Project::Loaded) {
            conn->write("Project loading");
        } else {
            const SourceMap infos = project->sources();
            for (const auto &it : infos) {
                if (match.isEmpty() || match.match(it.second.sourceFile())) {
                    if (flagsOnly) {
                        conn->write<128>("%s%s%s",
                                         it.second.sourceFile().constData(),
                                         splitLine ? "\n" : ": ",
                                         String::join(it.second.toCommandLine(0), splitLine ? '\n' : ' ').constData());
                    } else {
                        conn->write(it.second.toString());
                    }
                }
            }
        }
    } else {
        conn->write("No project");
    }
    conn->finish();
}

void Server::dumpCompletions(const QueryMessage &query, Connection *conn)
{
    if (mCompletionThread) {
        conn->write(mCompletionThread->dump());
    } else {
        conn->write("No completions");
    }
    conn->finish();
}

void Server::suspendFile(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project;
    const Match match = query.match();
    if (match.isEmpty() || match.pattern() == "clear") {
        project = currentProject();
    } else {
        project = projectForQuery(query);
    }
    if (!project) {
        conn->write("No project");
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
    } else {
        if (match.isEmpty()) {
            const Set<uint32_t> suspendedFiles = project->suspendedFiles();
            if (suspendedFiles.isEmpty()) {
                conn->write<512>("No files suspended for project %s", project->path().constData());
            } else {
                for (const auto &it : suspendedFiles)
                    conn->write<512>("%s is suspended", Location::path(it).constData());
            }
        } else {
            const Path p = query.match().pattern();
            if (p == "clear") {
                project->clearSuspendedFiles();
                conn->write<512>("No files are suspended");
            } else if (!p.isFile()) {
                conn->write<512>("%s doesn't seem to exist", p.constData());
            } else {
                const uint32_t fileId = Location::fileId(p);
                if (fileId) {
                    conn->write<512>("%s is no%s suspended", p.constData(),
                                     project->toggleSuspendFile(fileId) ? "w" : " longer");
                } else {
                    conn->write<512>("%s is not indexed", p.constData());
                }
            }
        }
    }
    conn->finish();
}

void Server::syncProject(const QueryMessage &qyery, Connection *conn)
{
    if (std::shared_ptr<Project> project = currentProject()) {
        project->startSync();
    } else {
        conn->write("No active project");
    }
    conn->finish();
}

void Server::handleJobRequestMessage(const JobRequestMessage &message, Connection *conn)
{
    if (debugMulti)
        error() << "got a request for" << message.numJobs() << "jobs from"
                << Rct::addrLookup(conn->client()->peerName())
                << mPending.size() << "potential jobs here";
    auto it = mPending.begin();
    List<std::shared_ptr<IndexerJob> > jobs;
    bool finished = true;
    while (it != mPending.end()) {
        std::shared_ptr<IndexerJob>& job = *it;
        if (job->unit->flags & (IndexerJob::CompleteLocal|IndexerJob::CompleteRemote)) {
            it = mPending.erase(it);
        } else if (!(job->unit->flags & IndexerJob::FromRemote) && !job->unit->preprocessed.isEmpty()) {
            assert(!job->process);

            if (mOptions.options & CompressionRemote && !(job->unit->flags & IndexerJob::PreprocessCompressed)) {
                StopWatch sw;
                job->unit->preprocessed = job->unit->preprocessed.compress();
                job->unit->flags |= IndexerJob::PreprocessCompressed;
                if (debugMulti)
                    error() << "Compressed" << job->unit->sourceFile << "in" << sw.elapsed() << "ms";
            }
            if (debugMulti)
                error() << "sending job" << job->unit->sourceFile << "to"
                        << Rct::addrLookup(conn->client()->peerName());

            jobs.append(job);
            it = mPending.erase(it);
            if (jobs.size() == message.numJobs()) {
                finished = false;
                break;
            }
        } else {
            if (debugMulti && job->unit->preprocessed.isEmpty()) {
                error() << "Didn't send job for" << job->unit->sourceFile << "since preprocessed is empty";

            }
            ++it;
        }
    }
    if (debugMulti) {
        error() << "Sending" << jobs.size() << "jobs to"
                << Rct::addrLookup(conn->client()->peerName())
                << "finished" << finished << "asked for" << message.numJobs();
    }
    conn->send(JobResponseMessage(jobs, mOptions.tcpPort, finished));
    conn->sendFinished().connect([jobs,this,finished](Connection*) {
            if (finished)
                mAnnounced = false;
            for (auto &job : jobs) {
                mProcessingJobs[job->id] = job;
                job->unit->flags |= IndexerJob::Remote;
                job->unit->flags &= ~IndexerJob::Rescheduled;
                job->started = Rct::monoMs();
                if (debugMulti)
                    error() << "Sent job" << job->unit->sourceFile;
            }
            startRescheduleTimer();
        });

    auto onError = [jobs,this](Connection*) {
        for (auto &job : jobs) {
            job->unit->flags &= ~IndexerJob::Rescheduled;
            mPending.append(job);
        }
    };
    conn->disconnected().connect(onError);
    conn->error().connect(onError);
    conn->finish();
}

void Server::handleJobResponseMessage(const JobResponseMessage &message, Connection *conn)
{
    mPendingJobRequests.remove(conn);
    const String host = conn->client()->peerName();
    const auto jobs = message.jobs(host);
    if (debugMulti)
        error() << "Got jobs from" << Rct::addrLookup(host)
                << jobs.size() << message.isFinished();
    for (const auto &job : jobs) {
        if (debugMulti)
            error() << "got indexer job with preprocessed" << job->unit->preprocessed.size() << job->unit->sourceFile;
        assert(job->unit->flags & IndexerJob::FromRemote);
        addJob(job);
    }
    if (message.isFinished()) {
        Remote *remote = mRemotes.take(host);
        if (remote) {
            Rct::LinkedList::remove(remote, mFirstRemote, mLastRemote);
            delete remote;
        }
    }
}

void Server::handleJobAnnouncementMessage(const JobAnnouncementMessage &message, Connection *conn)
{
    WorkScope scope;
    if (debugMulti)
        error() << "Getting job announcement from" << Rct::addrLookup(message.host());
    Remote *&remote = mRemotes[message.host()];
    if (!remote) {
        const String host = message.host().isEmpty() ? conn->client()->peerName() : message.host();
        remote = new Remote(host, message.port());;
    } else {
        Rct::LinkedList::remove(remote, mFirstRemote, mLastRemote);
    }
    Rct::LinkedList::insert(remote, mFirstRemote, mLastRemote);
}

void Server::handleProxyJobAnnouncementMessage(const ProxyJobAnnouncementMessage &message, Connection *conn)
{
    const JobAnnouncementMessage msg(conn->client()->peerName(), message.port());
    if (debugMulti) {
        error() << "Sending proxy job announcement" << Rct::addrLookup(conn->client()->peerName());
    }

    for (const auto &client : mClients) {
        if (client != conn)
            client->send(msg);
    }
    handleJobAnnouncementMessage(msg, conn);
}

void Server::handleClientMessage(const ClientMessage &, Connection *conn)
{
    error() << "Got a client connected from" << Rct::addrLookup(conn->client()->peerName());

    mClients.insert(conn);

    const ClientConnectedMessage msg(conn->client()->peerName());
    for (const auto &client : mClients) {
        if (client != conn)
            client->send(msg);
    }
    handleClientConnectedMessage(msg);
}

void Server::handleClientConnectedMessage(const ClientConnectedMessage &msg)
{
    if (debugMulti) {
        error() << "A new client joined the network" << Rct::addrLookup(msg.peer());
    }
    mAnnounced = false;
    work();
}

void Server::handleVisitFileMessage(const VisitFileMessage &message, Connection *conn)
{
    uint32_t fileId = 0;
    bool visit = false;

    std::shared_ptr<Project> project = mProjects.value(message.project());
    Path resolved;
    const uint64_t key = message.key();
    if (project && project->isValidJob(key)) {
        bool ok;
        resolved = message.file().resolved(Path::RealPath, Path(), &ok);
        if (ok) {
            fileId = Location::insertFile(resolved);
            visit = project->visitFile(fileId, resolved, key);
        }
    }
    VisitFileResponseMessage msg(fileId, resolved, visit);
    conn->send(msg);
}

void Server::restoreFileIds()
{
    const Path p = mOptions.dataDir + "fileids";
    bool clear = true;
    const String all = p.readAll();
    if (!all.isEmpty()) {
        Hash<Path, uint32_t> pathsToIds;
        Deserializer in(all);
        int version;
        in >> version;
        if (version == RTags::DatabaseVersion) {
            int size;
            in >> size;
            if (size != all.size()) {
                error("Refusing to load corrupted file %s", p.constData());
            } else {
                in >> pathsToIds;
                clear = false;
                Location::init(pathsToIds);
            }
        } else {
            error("%s has the wrong format. Got %d, expected %d. Can't restore anything",
                  p.constData(), version, RTags::DatabaseVersion);
        }
    }
    if (clear)
        clearProjects();
}

bool Server::saveFileIds() const
{
    if (!Path::mkdir(mOptions.dataDir)) {
        error("Can't create directory [%s]", mOptions.dataDir.constData());
        return false;
    }
    const Path p = mOptions.dataDir + "fileids";
    FILE *f = fopen(p.constData(), "w");
    if (!f) {
        error("Can't open file %s", p.constData());
        return false;
    }
    const Hash<Path, uint32_t> pathsToIds = Location::pathsToIds();
    Serializer out(f);
    out << static_cast<int>(RTags::DatabaseVersion);
    const int pos = ftell(f);
    out << static_cast<int>(0) << pathsToIds;
    const int size = ftell(f);
    fseek(f, pos, SEEK_SET);
    out << size;
    fclose(f);
    return true;
}

void Server::onUnload()
{
    std::shared_ptr<Project> cur = currentProject();
    for (const auto &it : mProjects) {
        if (it.second->state() != Project::Unloaded && it.second != cur && !it.second->isIndexing()) {
            it.second->unload();
        }
    }
}

template <typename T>
static inline bool slowContains(const LinkedList<T> &list, const T &t)
{
    for (T i : list) {
        if (i == t)
            return true;
    }
    return false;
}

void Server::onReschedule()
{
    const uint64_t now = Rct::monoMs();
    auto it = mProcessingJobs.begin();
    bool restartTimer = false;
    bool doWork = false;
    while (it != mProcessingJobs.end()) {
        const std::shared_ptr<IndexerJob>& job = it->second;
        if (job->unit->flags & (IndexerJob::CompleteRemote|IndexerJob::CompleteLocal)) {
            // this can happen if we complete it while we're sending it to a
            // remote. Should fix all of these
            it = mProcessingJobs.erase(it);
            continue;
        }
        if (!(job->unit->flags & (IndexerJob::Rescheduled|IndexerJob::RunningLocal)) && job->unit->flags & IndexerJob::Remote) {
            if (static_cast<int>(now - job->started) >= mOptions.rescheduleTimeout) {
                assert(!job->process);
                // this might never happen, reschedule this job
                // don't take it out of the mProcessingJobs list since the result might come back still
                // if (debugMulti)
                error() << "rescheduling job" << job->unit->sourceFile << job->id
                        << "it's been" << static_cast<double>(now - job->started) / 1000.0 << "seconds";
                job->unit->flags |= IndexerJob::Rescheduled;
                assert(!slowContains(mPending, job));
                mPending.push_back(job);
                doWork = true;
            } else {
                restartTimer = true;
            }
        }
        ++it;
    }
    if (restartTimer)
        startRescheduleTimer();
    if (doWork) {
        WorkScope scope;
    }
}

void Server::onMulticastReadyRead(const SocketClient::SharedPtr &socket,
                                  const String &ip,
                                  uint16_t port,
                                  Buffer &&in)
{
    const Buffer buffer = std::forward<Buffer>(in);
    if (debugMulti)
        error() << "Got some data from multicast socket" << buffer.size() << Rct::addrLookup(ip);
    const char *data = reinterpret_cast<const char*>(buffer.data());
    const int size = buffer.size();
    if (size == 2 && !strncmp(data, "s?", 2)) {
        String out;
        if (mServerConnection) {
            if (debugMulti)
                error() << Rct::addrLookup(ip) << "wants to know where the server is. I am connected to"
                        << String::format<128>("%s:%d",
                                               Rct::addrLookup(mServerConnection->client()->peerName()).constData(),
                                               mServerConnection->client()->port());

            Serializer serializer(out);
            serializer << mServerConnection->client()->peerName() << mServerConnection->client()->port();
        } else if (mOptions.jobServer.second) {
            if (debugMulti)
                error() << Rct::addrLookup(ip) << "wants to know where the server is. I have something in options"
                        << String::format<128>("%s:%d",
                                               Rct::addrLookup(mOptions.jobServer.first).constData(),
                                               mOptions.jobServer.second);
            Serializer serializer(out);
            serializer << mOptions.jobServer.first << mOptions.jobServer.second;
        } else if (mOptions.options & JobServer) {
            if (debugMulti)
                error() << Rct::addrLookup(ip) << "wants to know where the server is. I am the server" << mOptions.tcpPort;
            Serializer serializer(out);
            serializer << String() << mOptions.tcpPort;
        } else {
            if (debugMulti)
                error() << Rct::addrLookup(ip) << "wants to know where the server is but I don't know";
            return;
        }
        assert(!out.isEmpty());

        mMulticastSocket->writeTo(mOptions.multicastAddress, mOptions.multicastPort,
                                  reinterpret_cast<const unsigned char*>(out.constData()), out.size());
    } else if (!mServerConnection && !(mOptions.options & JobServer)) {
        Deserializer deserializer(data, size);
        deserializer >> mOptions.jobServer.first >> mOptions.jobServer.second;
        if (mOptions.jobServer.first.isEmpty())
            mOptions.jobServer.first = ip;
        if (debugMulti)
            error() << Rct::addrLookup(ip) << "tells me the server is to be found at"
                    << Rct::addrLookup(mOptions.jobServer.first);

        connectToServer();
    }
}

void Server::addJob(const std::shared_ptr<IndexerJob> &job)
{
    WorkScope scope;
    warning() << "adding job" << job->unit->sourceFile;
    assert(job);
    assert(!(job->unit->flags & (IndexerJob::CompleteRemote|IndexerJob::CompleteLocal)));
    mPending.push_back(job);
}

void Server::onLocalJobFinished(Process *process)
{
    WorkScope scope;
    assert(process);
    auto it = mLocalJobs.find(process);
    assert(it != mLocalJobs.end());
    std::shared_ptr<IndexerJob> &job = it->second.first;
    assert(job->process == process);
    error() << process->readAllStdErr() << process->readAllStdOut();
    if (debugMulti)
        error() << "job finished" << job->unit->sourceFile << String::format<16>("flags: 0x%x", job->unit->flags)
                << process->errorString() << process->readAllStdErr();
    if (job->unit->flags & IndexerJob::FromRemote) {
        error() << "Built remote job" << job->unit->sourceFile.toTilde() << "for"
                << Rct::addrLookup(job->destination)
                << "in" << (Rct::monoMs() - it->second.second) << "ms";
    }
    if (!(job->unit->flags & (IndexerJob::CompleteRemote|IndexerJob::CompleteLocal))
        && (process->returnCode() != 0 || !process->errorString().isEmpty())) {
        if (!(job->unit->flags & IndexerJob::Aborted))
            job->unit->flags |= IndexerJob::Crashed;
        job->unit->flags &= ~IndexerJob::RunningLocal;

        std::shared_ptr<Project> proj = project(job->project);
        if (proj && (proj->state() == Project::Loaded || proj->state() == Project::Syncing)) {
            std::shared_ptr<IndexData> data(new IndexData(job->unit->flags));
            data->key = job->unit->source.key();
            data->dependencies[job->unit->source.fileId].insert(job->unit->source.fileId);

            EventLoop::eventLoop()->registerTimer([data, job, proj](int) { proj->onJobFinished(data, job); },
                                                  500, Timer::SingleShot);
            // give it 500 ms before we try again
        }
    }
    job->process = 0;
    mProcessingJobs.erase(job->id);
    mLocalJobs.erase(it);
    EventLoop::deleteLater(process);
}

void Server::stopServers()
{
    Path::rm(mOptions.socketFile);
    mUnixServer.reset();
    mTcpServer.reset();
    mHttpServer.reset();
    mProjects.clear();
}

void Server::codeCompleteAt(const QueryMessage &query, Connection *conn)
{
    const String q = query.query();
    Deserializer deserializer(q);
    Path path;
    int line, column;
    deserializer >> path >> line >> column;
    path.resolve();
    std::shared_ptr<Project> project = projectForQuery(query);
    if (!project) {
        conn->write<128>("No project found for %s", path.constData());
        conn->finish();
        return;
    }
    const uint32_t fileId = Location::insertFile(path);
    const Source source = project->sources(fileId).value(query.buildIndex());
    if (source.isNull()) {
        conn->write<128>("No source found for %s", path.constData());
        conn->finish();
        return;
    }
    if (!mCompletionThread) {
        mCompletionThread = new CompletionThread(mOptions.completionCacheSize);
        mCompletionThread->start();
    }

    const Location loc(fileId, line, column);
    unsigned int flags = CompletionThread::None;
    if (query.type() == QueryMessage::PrepareCodeCompleteAt)
        flags |= CompletionThread::Refresh;
    if (query.flags() & QueryMessage::ElispList)
        flags |= CompletionThread::Elisp;
    if (!(query.flags() & QueryMessage::SynchronousCompletions)) {
        conn->finish();
        conn = 0;
    }
    error() << "Got completion" << String::format("%s:%d:%d", path.constData(), line, column);
    mCompletionThread->completeAt(source, loc, flags, query.unsavedFiles().value(path), conn);
}

static inline void drain(const SocketClient::SharedPtr &sock)
{
    Buffer data = std::move(sock->takeBuffer());
    (void)data;
}

void Server::onHttpClientReadyRead(const SocketClient::SharedPtr &socket)
{
    auto &log = mHttpClients[socket];
    if (!log) {
        static const char *statsRequestLine = "GET /stats HTTP/1.1\r\n";
        static const size_t statsLen = strlen(statsRequestLine);
        const size_t len = socket->buffer().size();
        if (len >= statsLen) {
            if (!memcmp(socket->buffer().data(), statsRequestLine, statsLen)) {
                static const char *response = ("HTTP/1.1 200 OK\r\n"
                                               "Cache: no-cache\r\n"
                                               "Cache-Control: private\r\n"
                                               "Pragma: no-cache\r\n"
                                               "Content-Type: text/event-stream\r\n\r\n");
                static const int responseLen = strlen(response);
                socket->write(reinterpret_cast<const unsigned char*>(response), responseLen);
                log.reset(new HttpLogObject(RTags::Statistics, socket));
                ::drain(socket);
            } else {
                socket->close();
            }
        }
    } else {
        ::drain(socket);
    }
}

void Server::connectToServer()
{
    enum { ServerReconnectTimer = 5000 };

    debug() << "connectToServer" << mConnectToServerFailures;
    mConnectToServerTimer.stop();
    assert(!(mOptions.options & JobServer));
    if (mServerConnection)
        return;
    if (!mOptions.jobServer.second) {
        if (mMulticastSocket) {
            const unsigned char data[] = { 's', '?' };
            mMulticastSocket->writeTo(mOptions.multicastAddress, mOptions.multicastPort, data, 2);
            mConnectToServerTimer.restart(ServerReconnectTimer * ++mConnectToServerFailures);
        }
        return;
    }

    mServerConnection = new Connection;
    mServerConnection->disconnected().connect([this](Connection *conn) {
            assert(conn == mServerConnection);
            (void)conn;
            EventLoop::deleteLater(mServerConnection);
            mServerConnection = 0;
            mConnectToServerTimer.restart(ServerReconnectTimer * ++mConnectToServerFailures);
            warning() << "Disconnected from server" << conn->client()->peerName();
        });
    mServerConnection->connected().connect([this](Connection *conn) {
            mConnectToServerFailures = 0;
            assert(conn == mServerConnection);
            (void)conn;
            if (!mServerConnection->send(ClientMessage())) {
                mServerConnection->close();
                EventLoop::deleteLater(mServerConnection);
                mServerConnection = 0;
                mConnectToServerTimer.restart(ServerReconnectTimer * ++mConnectToServerFailures);
                error() << "Couldn't send logoutputmessage";
            } else {
                error() << "Connected to server" << Rct::addrLookup(conn->client()->peerName());
            }
        });
    mServerConnection->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));

    if (!mServerConnection->connectTcp(mOptions.jobServer.first, mOptions.jobServer.second)) {
        delete mServerConnection;
        mServerConnection = 0;
        mConnectToServerTimer.restart(ServerReconnectTimer * ++mConnectToServerFailures);
        error() << "Failed to connect to server" << mOptions.jobServer;
    }
}

void Server::dumpJobs(Connection *conn)
{
    if (!mPending.isEmpty()) {
        conn->write("Pending:");
        for (const auto &job : mPending) {
            conn->write<128>("%s: 0x%x %s",
                             job->unit->sourceFile.constData(),
                             job->unit->flags,
                             IndexerJob::dumpFlags(job->unit->flags).constData());
        }
    }
    if (!mLocalJobs.isEmpty()) {
        conn->write("Local:");
        for (const auto &job : mLocalJobs) {
            conn->write<128>("%s: 0x%x %s",
                             job.second.first->unit->sourceFile.constData(),
                             job.second.first->unit->flags,
                             IndexerJob::dumpFlags(job.second.first->unit->flags).constData());
        }
    }
    if (!mProcessingJobs.isEmpty()) {
        conn->write("Processing:");
        for (const auto &job : mProcessingJobs) {
            conn->write<128>("%s: 0x%x %s",
                             job.second->unit->sourceFile.constData(),
                             job.second->unit->flags,
                             IndexerJob::dumpFlags(job.second->unit->flags).constData());
        }
    }

    if (mThreadPool && (mThreadPool->backlogSize() || mThreadPool->busyThreads())) {
        conn->write<128>("Preprocessing:\nactive %d pending %d", mThreadPool->busyThreads(), mThreadPool->backlogSize());
    }
}

void Server::startRescheduleTimer()
{
    if (!mRescheduleTimer.isRunning())
        mRescheduleTimer.restart(mOptions.rescheduleTimeout, Timer::SingleShot);
}

void Server::work()
{
    mWorkPending = false;
    int jobs = mOptions.jobCount;
    if (mThreadPool) {
        int pending = std::min(mPendingPreprocessJobs.size(),
                               mOptions.maxPendingPreprocessSize - (mThreadPool->backlogSize() + mThreadPool->busyThreads() + mPending.size()));
        while (pending > 0) {
            std::shared_ptr<PreprocessJob> job = mPendingPreprocessJobs.front();
            mPendingPreprocessJobs.pop_front();
            mThreadPool->start(job);
            --pending;
        }

        jobs -= (mThreadPool->busyThreads() + mThreadPool->backlogSize());
    }

    // printf("WORK CALLED: preprocessing %d backlog %d pending preprocess %d active jobs %d pending jobs %d\n",
    //        mThreadPool ? mThreadPool->busyThreads() : 0, mThreadPool ? mThreadPool->backlogSize() : 0,
    //        mPendingPreprocessJobs.size(), mLocalJobs.size(), mLocalJobs.size());

    jobs -= mLocalJobs.size();
    int pendingJobRequestCount = 0;
    for (const auto &it : mPendingJobRequests) {
        pendingJobRequestCount += it.second;
    }
    jobs -= pendingJobRequestCount;

    if (debugMulti) {
        Log log(Error);
        log << "Working. Open slots" << std::max(0, jobs);
        if (mThreadPool) {
            log << "preprocessing" << mThreadPool->busyThreads()
                << "backlog" << mThreadPool->backlogSize()
                << "pending" << mPendingPreprocessJobs.size() << "\n";
        }
        log << "active jobs" << mLocalJobs.size()
            << "pending jobs" << mPending.size()
            << "We have" << (mAnnounced ? "announced" : "not announced") << "\n";

        log << "pending job requests" << pendingJobRequestCount;
        int idx = 0;
        for (Remote *remote = mFirstRemote; remote; remote = remote->next) {
            log << "remote" << ++idx << "of" << mRemotes.size()
                << Rct::addrLookup(remote->host);
        }
    }
    if (mOptions.options & NoLocalCompiles)
        jobs = std::min(jobs, 0);

    if (jobs <= 0 && !hasServer())
        return;

    auto it = mPending.begin();
    int announcables = 0;
    while (it != mPending.end()) {
        auto job = *it;
        assert(job);
        if (job->unit->flags & (IndexerJob::CompleteLocal|IndexerJob::CompleteRemote)) {
            it = mPending.erase(it);
            continue;
        }

        if (jobs > 0) {
            if (!(job->unit->flags & IndexerJob::FromRemote))
                mProcessingJobs[job->id] = job;
            job->unit->flags &= ~IndexerJob::Rescheduled;
            it = mPending.erase(it);
            --jobs;
            if (job->launchProcess()) {
                if (debugMulti)
                    error() << "started job locally for" << job->unit->sourceFile << job->id;
                mLocalJobs[job->process] = std::make_pair(job, Rct::monoMs());
                assert(job->process);
                job->process->finished().connect(std::bind(&Server::onLocalJobFinished, this,
                                                           std::placeholders::_1));
            } else {
                mLocalJobs[job->process] = std::make_pair(job, Rct::monoMs());
                EventLoop::eventLoop()->callLater(std::bind(&Server::onLocalJobFinished, this, std::placeholders::_1), job->process);
            }
        } else {
            if (!(job->unit->flags & IndexerJob::FromRemote)) {
                if (!project(job->project)) {
                    it = mPending.erase(it);
                    continue;
                }
                ++announcables;
            }

            ++it;
        }
    }

    if (!hasServer())
        return;

    if (!mAnnounced && announcables) {
        mAnnounced = true;
        if (debugMulti)
            error() << "announcing because we have" << announcables << "announcables";
        if (mServerConnection) {
            mServerConnection->send(ProxyJobAnnouncementMessage(mOptions.tcpPort));
        } else {
            // Don't pass a host name on the original announcement message,
            // the receiver will derive it
            const JobAnnouncementMessage msg(String(), mOptions.tcpPort);
            for (const auto &client : mClients) {
                client->send(msg);
            }
        }
    } else if (debugMulti) {
        error() << (mAnnounced ? "Already announced" : "Nothing to announce");
    }

    if (jobs <= 0)
        return;

    int remoteCount = mRemotes.size();
    while (remoteCount) {
        // Could cache these connections. We could at least use the server
        // connection if it's the server we're connecting to.
        Remote *remote = mFirstRemote;
        if (remote != mLastRemote) {
            if (remote == mFirstRemote) {
                assert(!remote->prev);
                assert(remote->next);
                mFirstRemote = remote->next;
                mFirstRemote->prev = 0;
            } else {
                assert(remote->prev);
                remote->prev->next = remote->next;
                remote->next->prev = remote->prev;
            }
            remote->next = 0;
            remote->prev = mLastRemote;
            mLastRemote->next = remote;
            mLastRemote = remote;
        }

        --remoteCount;
        Connection *conn = new Connection;
        if (debugMulti) {
            error() << "We can grab" << jobs << "jobs, trying"
                    << Rct::addrLookup(remote->host);
        }
        if (!conn->connectTcp(remote->host, remote->port)) {
            error() << "Failed to connect to" << remote->host << remote->port;
            delete conn;
        } else {
            if (debugMulti)
                error() << "asking" << Rct::addrLookup(remote->host)
                        << "for" << jobs << "jobs";
            conn->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
            conn->disconnected().connect(std::bind(&Server::onConnectionDisconnected, this, std::placeholders::_1));
            conn->finished().connect(std::bind([this, conn]() { mPendingJobRequests.remove(conn); conn->close(); EventLoop::deleteLater(conn); }));
            conn->send(JobRequestMessage(jobs));
            assert(!mPendingJobRequests.contains(conn)); // ### is this certain?
            mPendingJobRequests[conn] = jobs;
            break;
        }
    }
}

Server::WorkScope::WorkScope()
{
    Server *s = Server::instance();
    assert(s);
    work = !s->mWorkPending;
    if (work)
        s->mWorkPending = true;
}

Server::WorkScope::~WorkScope()
{
    if (work) {
        Server *s = Server::instance();
        s->work();
    }
}
bool Server::hasServer() const
{
    if (mOptions.options & JobServer)
        return true;
    if (mOptions.options & NoJobServer)
        return false;
    return mServerConnection;
}
