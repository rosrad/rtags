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

#ifndef IndexerJob_h
#define IndexerJob_h

#include "RTags.h"
#include "Unit.h"
#include <rct/Hash.h>
#include <rct/ThreadPool.h>
#include <rct/StopWatch.h>
#include <rct/Hash.h>
#include "Source.h"

class Process;
class IndexerJob : public std::enable_shared_from_this<IndexerJob>
{
public:
    enum Flag {
        None = 0x0000,
        Dirty = 0x0001,
        Compile = 0x0002,
        Type_Mask = Dirty|Compile,
        FromRemote = 0x0010, // this job originated on another machine, we're running it to be nice
        Remote = 0x0020, // this job represents a locally spawned index that currently runs on some other machine
        Rescheduled = 0x0040,
        RunningLocal = 0x0080,
        Crashed = 0x0100,
        Aborted = 0x0200,
        CompleteLocal = 0x0400,
        CompleteRemote = 0x0800,
        PreprocessCompressed = 0x1000,
        HighPriority = 0x2000
    };

    static String dumpFlags(unsigned int);

    IndexerJob(const Path &project, const std::shared_ptr<Unit> &unit);
    IndexerJob();
    ~IndexerJob();

    bool launchProcess();
    bool update(const std::shared_ptr<Unit> &unit);
    void abort();
    void encode(Serializer &serializer);

    String destination;
    uint16_t port;
    Path project;
    Set<uint32_t> visited;
    Process *process;
    Hash<uint32_t, Path> blockedFiles; // only used for remote jobs
    uint64_t id, started;
    std::shared_ptr<Unit> unit;

    static uint64_t nextId;
};

#endif
