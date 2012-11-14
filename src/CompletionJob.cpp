#include "CompletionJob.h"
#include "IndexerJob.h"
#include "EventLoop.h"

CompletionJob::CompletionJob(const shared_ptr<Project> &project)
    : Job(WriteBuffered|WriteUnfiltered, project), mIndex(0), mUnit(0), mLine(-1), mColumn(-1), mPos(-1)
{
}

void CompletionJob::init(CXIndex index, CXTranslationUnit unit, const Path &path, const List<ByteArray> &args,
                         int line, int column, int pos, const ByteArray &unsaved)
{
    mIndex = index;
    mUnit = unit;
    mPath = path;
    mArgs = args;
    mLine = line;
    mPos = pos;
    mColumn = column;
    mUnsaved = unsaved;
}

static inline bool isPartOfSymbol(char ch)
{
    return isalnum(ch) || ch == '_';
}

static inline CXCursor findContainer(CXCursor cursor)
{
    do {
        switch (clang_getCursorKind(cursor)) {
        case CXCursor_FunctionDecl:
        case CXCursor_FunctionTemplate:
        case CXCursor_CXXMethod:
        case CXCursor_Constructor:
        case CXCursor_Destructor:
        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_StructDecl:
        case CXCursor_Namespace:
            return cursor;
        default:
            break;
        }
        cursor = clang_getCursorSemanticParent(cursor);
    } while (!clang_isInvalid(clang_getCursorKind(cursor)));

    assert(clang_equalCursors(cursor, clang_getNullCursor()));

    return cursor;
}

static inline ByteArray fullyQualifiedName(CXCursor cursor)
{
    ByteArray ret;
    ret.reserve(128);
    CXCursorKind kind = clang_getCursorKind(cursor);
    const CXCursor orig = cursor;

    do {
        if (!ret.isEmpty())
            ret.prepend("::");
        ret.prepend(RTags::eatString(clang_getCursorDisplayName(cursor)));

        bool done = true;
        switch (kind) {
        case CXCursor_CXXMethod:
        case CXCursor_Constructor:
        case CXCursor_FunctionDecl:
        case CXCursor_Destructor:
        case CXCursor_FieldDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_Namespace:
        case CXCursor_ClassDecl:
        case CXCursor_StructDecl:
        case CXCursor_EnumConstantDecl:
        case CXCursor_EnumDecl:
        case CXCursor_TypedefDecl:
            done = false;
            break;
        default:
            break;
        }
        if (done)
            break;

        cursor = clang_getCursorSemanticParent(cursor);
        kind = clang_getCursorKind(cursor);
    } while (RTags::isContainer(kind));

    switch (clang_getCursorKind(orig)) {
    case CXCursor_FieldDecl:
    case CXCursor_ParmDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_VarDecl: {
        const CXCursor child = RTags::findFirstChild(orig);
        kind = clang_getCursorKind(child);
        switch (kind) {
        case CXCursor_TypeRef:
        case CXCursor_TemplateRef: {
            const CXStringScope str = clang_getCursorSpelling(child);
            if (str.data()) {
                const char *cstr = str.data();
                if (!strncmp(cstr, "class ", 6)) {
                    cstr += 6;
                } else if (!strncmp(cstr, "struct ", 7)) {
                    cstr += 7;
                }
                ret.prepend(' ');
                ret.prepend(cstr);
            }
            break; }
        default:
            // error() << "Got something else here" << orig << child;
            break;
        }
        break; }
    default:
        break;
    }

    return ret;
}

struct CompletionNode
{
    ByteArray completion, signature;
    int priority, distance;
    enum DistanceType {
        Before,
        After,
        None
    } distanceType;
};

static int compareCompletionNode(const void *left, const void *right)
{
    const CompletionNode *l = reinterpret_cast<const CompletionNode*>(left);
    const CompletionNode *r = reinterpret_cast<const CompletionNode*>(right);
    if (l->priority != r->priority)
        return l->priority < r->priority ? -1 : 1;
    if (l->distanceType != r->distanceType)
        return l->distanceType < r->distanceType ? -1 : 1;
    if (l->distance != r->distance)
        return l->distance < r->distance ? -1 : 1;
    return strcmp(l->completion.constData(), r->completion.constData());
}

void CompletionJob::execute()
{
    CXUnsavedFile unsavedFile = { mUnsaved.isEmpty() ? 0 : mPath.constData(),
                                  mUnsaved.isEmpty() ? 0 : mUnsaved.constData(),
                                  static_cast<unsigned long>(mUnsaved.size()) };

    CXCodeCompleteResults *results = clang_codeCompleteAt(mUnit, mPath.constData(), mLine, mColumn,
                                                          &unsavedFile, mUnsaved.isEmpty() ? 0 : 1,
                                                          clang_defaultCodeCompleteOptions());

    if (results) {
        CompletionNode *nodes = new CompletionNode[results->NumResults];
        int nodeCount = 0;
        for (unsigned i = 0; i < results->NumResults; ++i) {
            const CXCursorKind kind = results->Results[i].CursorKind;
            if (kind == CXCursor_Destructor)
                continue;

            const CXCompletionString &string = results->Results[i].CompletionString;
            const CXAvailabilityKind availabilityKind = clang_getCompletionAvailability(string);
            if (availabilityKind != CXAvailability_Available)
                continue;

            const int priority = clang_getCompletionPriority(string);
            if (priority >= 75)
                continue;

            CompletionNode &node = nodes[nodeCount];
            node.priority = priority;
            node.signature.reserve(256);
            const int chunkCount = clang_getNumCompletionChunks(string);
            bool ok = true;
            for (int j=0; j<chunkCount; ++j) {
                const CXCompletionChunkKind chunkKind = clang_getCompletionChunkKind(string, j);
                if (chunkKind == CXCompletionChunk_TypedText) {
                    node.completion = RTags::eatString(clang_getCompletionChunkText(string, j));
                    if (node.completion.size() > 8 && node.completion.startsWith("operator") && !isPartOfSymbol(node.completion.at(8))) {
                        ok = false;
                        break;
                    }
                    node.signature.append(node.completion);
                } else {
                    node.signature.append(RTags::eatString(clang_getCompletionChunkText(string, j)));
                    if (chunkKind == CXCompletionChunk_ResultType)
                        node.signature.append(' ');
                }
            }

            int ws = node.completion.size() - 1;
            while (ws >= 0 && isspace(node.completion.at(ws)))
                --ws;
            if (ok && ws >= 0) {
                node.completion.truncate(ws + 1);
                int pos =  mUnsaved.lastIndexOf(node.completion, mPos - 1);
                if (pos != -1) {
                    node.distanceType = CompletionNode::Before;
                    node.distance = mPos - pos;
                } else {
                    pos = mUnsaved.indexOf(node.completion, mPos);
                    if (pos == -1) {
                        node.distanceType = CompletionNode::None;
                        node.distance = -1;
                    } else {
                        node.distanceType = CompletionNode::After;
                        node.distance = pos - mPos;
                    }
                }
                ++nodeCount;
            } else {
                node.completion.clear();
                node.signature.clear();
            }
        }
        if (nodeCount) {
            qsort(nodes, nodeCount, sizeof(CompletionNode), compareCompletionNode);
            write<128>("`%s %s", nodes[0].completion.constData(), nodes[0].signature.constData());
            for (int i=1; i<nodeCount; ++i) {
                write<128>("%s %s", nodes[i].completion.constData(), nodes[i].signature.constData());
            }
        }

        delete[] nodes;

        clang_disposeCodeCompleteResults(results);
        project()->indexer->addToCache(mPath, mArgs, mIndex, mUnit);
    }
    mFinished(mPath);
}