//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RecyclerChecker.h"

MainVisitor::MainVisitor(
        CompilerInstance& compilerInstance, ASTContext& context, bool fix)
    : _compilerInstance(compilerInstance), _context(context),
     _fix(fix), _fixed(false), _barrierTypeDefined(false)
{
    if (_fix)
    {
        _rewriter.setSourceMgr(compilerInstance.getSourceManager(),
                               compilerInstance.getLangOpts());
    }
}

bool MainVisitor::VisitCXXRecordDecl(CXXRecordDecl* recordDecl)
{
    std::string typeName = recordDecl->getQualifiedNameAsString();

    // Ignore (system/non-GC types) before seeing "Memory::NoWriteBarrierField"
    if (!_barrierTypeDefined)
    {
        if (typeName != "Memory::NoWriteBarrierField")
        {
            return true;
        }

        _barrierTypeDefined = true;
    }

    if (!recordDecl->hasDefinition())
    {
        return true;
    }

    bool hasUnbarrieredPointer = false;
    bool hasBarrieredField = false;

    for (auto field : recordDecl->fields())
    {
        const QualType qualType = field->getType();
        const Type* type = qualType.getTypePtr();

        auto fieldTypeName = qualType.getAsString();
        if (StartsWith(fieldTypeName, "typename WriteBarrierFieldTypeTraits") ||
            StartsWith(fieldTypeName, "const typename WriteBarrierFieldTypeTraits"))
        {
            // Note this only indicates the class is write-barrier annotated
            hasBarrieredField = true;
        }
        else if (type->isPointerType())
        {
            hasUnbarrieredPointer = true;
        }
        else if (type->isCompoundType())
        {
            // If the field is a compound type,
            // check if it is a fully barriered type or
            // has unprotected pointer fields
            if (Contains(_pointerClasses, fieldTypeName))
            {
                hasUnbarrieredPointer = true;
            }
            else if (Contains(_barrieredClasses, fieldTypeName))
            {
                hasBarrieredField = true;
            }
        }
    }

    if (hasUnbarrieredPointer)
    {
        _pointerClasses.insert(typeName);
    }
    else if (hasBarrieredField)
    {
        _barrieredClasses.insert(typeName);
    }

    return true;
}

void MainVisitor::ProcessUnbarriedFields(CXXRecordDecl* recordDecl)
{
    const auto& sourceMgr = _compilerInstance.getSourceManager();
    DiagnosticsEngine& diagEngine = _context.getDiagnostics();
    const unsigned diagID = diagEngine.getCustomDiagID(
        DiagnosticsEngine::Error, "Unbarried field");

    for (auto field : recordDecl->fields())
    {
        const QualType qualType = field->getType();
        string fieldTypeName = qualType.getAsString();

        bool report = !StartsWith(fieldTypeName, "typename WriteBarrierFieldTypeTraits")
                && !StartsWith(fieldTypeName, "const typename WriteBarrierFieldTypeTraits")
                && field->getNameAsString().length() > 0;  // no-field-name union
        if (report)
        {
            SourceLocation location = field->getLocStart();
            if (this->_fix)
            {
                const char* begin = sourceMgr.getCharacterData(location);
                const char* end = begin;

                if (MatchType(fieldTypeName, begin, &end))
                {
                    _rewriter.ReplaceText(
                        location, end - begin,
                        GetFieldTypeAnnotation(qualType) + string(begin, end) +
                             (*end == ' ' ? ")" : ") "));
                    _fixed = true;
                    continue;
                }

                Log::errs() << "Fail to fix: " << fieldTypeName << " "
                            << field->getNameAsString()  << "\n";
            }

            diagEngine.Report(location, diagID);
        }
    }
}

static bool SkipSpace(const char*& p)
{
    if (*p == ' ')
    {
        ++p;
        return true;
    }

    return false;
}

template <size_t N>
static bool SkipPrefix(const char*& p, const char (&prefix)[N])
{
    if (StartsWith(p, prefix))
    {
        p += N - 1; // skip
        return true;
    }

    return false;
}

static bool SkipPrefix(const char*& p, const string& prefix)
{
    if (StartsWith(p, prefix))
    {
        p += prefix.length(); // skip
        return true;
    }

    return false;
}

static bool SkipTemplateParameters(const char*& p)
{
    if (*p == '<')
    {
        ++p;
        int left = 1;

        while (left && *p)
        {
            switch (*p++)
            {
                case '<': ++left; break;
                case '>': --left; break;
            }
        }

        return true;
    }

    return false;
}

bool MainVisitor::MatchType(const string& type, const char* source, const char** pSourceEnd)
{
    // try match type in source directly (clang "bool" type is "_Bool")
    if (SkipPrefix(source, type) || (type == "_Bool" && SkipPrefix(source, "bool")))
    {
        *pSourceEnd = source;
        return true;
    }

    const char* p = type.c_str();
    while (*p && *source)
    {
        if (SkipSpace(p) || SkipSpace(source))
        {
            continue;
        }

#define SKIP_EITHER_PREFIX(prefix) \
            (SkipPrefix(p, prefix) || SkipPrefix(source, prefix))
        if (SKIP_EITHER_PREFIX("const ") ||
            SKIP_EITHER_PREFIX("class ") ||
            SKIP_EITHER_PREFIX("struct ") ||
            SKIP_EITHER_PREFIX("union ") ||
            SKIP_EITHER_PREFIX("enum "))
        {
            continue;
        }
#undef SKIP_EITHER_PREFIX

        // type may contain [...] array specifier, while source has it after field name
        if (*p == '[')
        {
            while (*p && *p++ != ']');
            continue;
        }

        // skip <...> in both
        if (SkipTemplateParameters(p) || SkipTemplateParameters(source))
        {
            continue;
        }

        // type may contain fully qualified name but source may or may not
        const char* pSkipScopeType = strstr(p, "::");
        if (pSkipScopeType && !memchr(p, ' ', pSkipScopeType - p))
        {
            pSkipScopeType += 2;
            if (strncmp(source, p, pSkipScopeType - p) == 0)
            {
                source += pSkipScopeType - p;
            }
            p = pSkipScopeType;
            continue;
        }

        if (*p == *source)
        {
            while (*p && *source && *p == *source && !strchr("<>", *p))
            {
                ++p, ++source;
            }
            continue;
        }

        if (*p != *source)
        {
            return false;  // mismatch
        }
    }

    if (!*p && *source)  // type match completed and having remaining source
    {
        *pSourceEnd = source;
        return true;
    }

    return false;
}

const char* MainVisitor::GetFieldTypeAnnotation(QualType qtype)
{
    if (qtype->isPointerType())
    {
        auto type = qtype->getUnqualifiedDesugaredType()->getPointeeType().getTypePtr();
        const auto& i = _allocationTypes.find(type);
        if (i != _allocationTypes.end()
            && i->second == AllocationTypes::NonRecycler)
        {
            return "FieldNoBarrier(";
        }
    }

    return "Field(";
}

bool MainVisitor::VisitFunctionDecl(FunctionDecl* functionDecl)
{
    if (functionDecl->hasBody())
    {
        CheckAllocationsInFunctionVisitor visitor(this, functionDecl);

        visitor.TraverseDecl(functionDecl);
    }

    return true;
}

void MainVisitor::RecordAllocation(QualType qtype, AllocationTypes allocationType)
{
    auto type = qtype->getCanonicalTypeInternal().getTypePtr();
    _allocationTypes[type] |= allocationType;
}

void MainVisitor::RecordRecyclerAllocation(const string& allocationFunction, const string& type)
{
    _allocatorTypeMap[allocationFunction].insert(type);
}

template <class Set, class DumpItemFunc>
void MainVisitor::dump(const char* name, const Set& set, const DumpItemFunc& func)
{
    Log::outs() << "-------------------------\n\n";
    Log::outs() << name << "\n";
    Log::outs() << "-------------------------\n\n";
    for (auto item : set)
    {
        func(Log::outs(), item);
    }
    Log::outs() << "-------------------------\n\n";
}

template <class Item>
void MainVisitor::dump(const char* name, const set<Item>& set)
{
    dump(name, set, [](raw_ostream& out, const Item& item)
    {
        out << "  " << item << "\n";
    });
}

void MainVisitor::dump(const char* name, const set<const Type*> set)
{
    dump(name, set, [&](raw_ostream& out, const Type* type)
    {
        out << "  " << QualType(type, 0).getAsString() << "\n";
    });
}

void MainVisitor::Inspect()
{
#define Dump(coll) dump(#coll, _##coll)
    Dump(pointerClasses);
    Dump(barrieredClasses);

    Log::outs() << "Recycler allocations\n";
    for (auto item : _allocatorTypeMap)
    {
        dump(item.first.c_str(), item.second);
    }

    // TODO: Also add any type that has wb annotations to barrierTypes to check completeness
    set<const Type*> barrierTypes;
    for (auto item : _allocationTypes)
    {
        if (item.second & AllocationTypes::WriteBarrier)
        {
            barrierTypes.insert(item.first);
        }
    }
    dump("WriteBarrier allocation types", barrierTypes);

    // Examine all barrierd types. They should be fully wb annotated.
    for (auto type: barrierTypes)
    {
        auto r = type->getCanonicalTypeInternal()->getAsCXXRecordDecl();
        if (r)
        {
            auto typeName = r->getQualifiedNameAsString();
            ProcessUnbarriedFields(r);
        }
    }

#undef Dump
}

bool MainVisitor::ApplyFix()
{
    return _fixed ? _rewriter.overwriteChangedFiles() : false;
}

static AllocationTypes CheckAllocationType(const CXXStaticCastExpr* castNode)
{
    QualType targetType = castNode->getTypeAsWritten();
    if (const IdentifierInfo* info = targetType.getBaseTypeIdentifier())
    {
        return info->getName().equals("Recycler") ?
                AllocationTypes::Recycler : AllocationTypes::NonRecycler;
    }
    else
    {
        // Unknown template dependent allocator types
        return AllocationTypes::Unknown;
    }
}

bool CheckAllocationsInFunctionVisitor::VisitCXXNewExpr(CXXNewExpr* newExpr)
{
    if (newExpr->getNumPlacementArgs() > 1)
    {
        const Expr* firstArgNode = newExpr->getPlacementArg(0);

        // Check if the first argument to new is a static cast
        // AllocatorNew in Chakra always does a static_cast to the AllocatorType
        CXXStaticCastExpr* castNode = nullptr;
        if (firstArgNode != nullptr &&
            (castNode = const_cast<CXXStaticCastExpr*>(dyn_cast<CXXStaticCastExpr>(firstArgNode))))
        {
            QualType allocatedType = newExpr->getAllocatedType();
            auto allocationType = CheckAllocationType(castNode);

            if (allocationType == AllocationTypes::Recycler)  // Recycler allocation
            {
                const Expr* secondArgNode = newExpr->getPlacementArg(1);

                // Chakra has two types of allocating functions- throwing and non-throwing
                // However, recycler allocations are always throwing, so the second parameter
                // should be the address of the allocator function
                auto unaryNode = cast<UnaryOperator>(secondArgNode);
                if (unaryNode != nullptr && unaryNode->getOpcode() == UnaryOperatorKind::UO_AddrOf)
                {
                    Expr* subExpr = unaryNode->getSubExpr();
                    if (DeclRefExpr* declRef = cast<DeclRefExpr>(subExpr))
                    {
                        string allocatedTypeStr = allocatedType.getAsString();
                        auto declNameInfo = declRef->getNameInfo();
                        auto allocationFunctionStr = declNameInfo.getName().getAsString();
                        _mainVisitor->RecordRecyclerAllocation(allocationFunctionStr, allocatedTypeStr);

                        if (allocationFunctionStr.find("WithBarrier") != string::npos)
                        {
                            Log::outs() << "In \"" << _functionDecl->getQualifiedNameAsString() << "\"\n";
                            Log::outs() << "  Allocating \"" << allocatedTypeStr << "\" in write barriered memory\n";

                            // Recycler write barrier allocation
                            allocationType = AllocationTypes::WriteBarrier;
                        }
                    }
                    else
                    {
                        Log::errs() << "ERROR: (internal) Expected DeclRefExpr:\n";
                        subExpr->dump();
                    }
                }
                else
                {
                    Log::errs() << "ERROR: (internal) Expected unary node:\n";
                    secondArgNode->dump();
                }
            }

            _mainVisitor->RecordAllocation(allocatedType, allocationType);
        }
    }

    return true;
}

void RecyclerCheckerConsumer::HandleTranslationUnit(ASTContext& context)
{
    MainVisitor mainVisitor(_compilerInstance, context, _fix);
    mainVisitor.TraverseDecl(context.getTranslationUnitDecl());

    mainVisitor.Inspect();
    mainVisitor.ApplyFix();
}

std::unique_ptr<ASTConsumer> RecyclerCheckerAction::CreateASTConsumer(
    CompilerInstance& compilerInstance, llvm::StringRef)
{
    return llvm::make_unique<RecyclerCheckerConsumer>(compilerInstance, _fix);
}

bool RecyclerCheckerAction::ParseArgs(
    const CompilerInstance& compilerInstance, const std::vector<std::string>& args)
{
    for (auto i = args.begin(); i != args.end(); i++)
    {
        if (*i == "-verbose")
        {
            Log::SetLevel(Log::LogLevel::Verbose);
        }
        else if (*i == "-fix")
        {
            this->_fix = true;
        }
        else
        {
            Log::errs()
                << "ERROR: Unrecognized check-recycler option: " << *i << "\n"
                << "Supported options:\n"
                << "  -fix          Fix missing write barrier annotations"
                << "  -verbose      Log verbose messages\n";
            return false;
        }
    }

    return true;
}

static FrontendPluginRegistry::Add<RecyclerCheckerAction> recyclerPlugin(
    "check-recycler", "Checks the recycler allocations");