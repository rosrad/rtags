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
#include <rct/Hash.h>
#include <rct/ThreadPool.h>
#include <rct/StopWatch.h>
#include <rct/Hash.h>
#include <rct/Process.h>
#include "Source.h"

class IndexerJob : public std::enable_shared_from_this<IndexerJob>
{
public:
    enum Flag {
        None = 0x000,
        Dirty = 0x001,
        Compile = 0x002,
        Type_Mask = Dirty|Compile,
        Running = 0x010,
        Crashed = 0x020,
        Aborted = 0x040,
        Complete = 0x080,
        HighPriority = 0x100
    };

    static String dumpFlags(unsigned int);

    IndexerJob(const Source &source, uint32_t flags,
               const Path &project, const Hash<Path, String> &unsavedFiles);
    IndexerJob();

    bool launchProcess();
    bool update(const Source &source, uint32_t flags);
    void abort();
    void encode(Serializer &serializer) const;

    String destination;
    Source source;
    Path sourceFile;
    uint32_t flags;
    Path project;
    Hash<Path, String> unsavedFiles;
    Set<uint32_t> visited;
    Process *process;
    uint64_t started;
};

#endif
