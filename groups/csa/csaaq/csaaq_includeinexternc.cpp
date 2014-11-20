// csaaq_includeinexternc.cpp                                         -*-C++-*-

#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Token.h>
#include <clang/Tooling/Refactoring.h>
#include <csabase_analyser.h>
#include <csabase_clang.h>
#include <csabase_config.h>
#include <csabase_debug.h>
#include <csabase_diagnostic_builder.h>
#include <csabase_filenames.h>
#include <csabase_location.h>
#include <csabase_ppobserver.h>
#include <csabase_registercheck.h>
#include <csabase_util.h>
#include <csabase_visitor.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Regex.h>
#include <llvm/Support/raw_ostream.h>
#include <stddef.h>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <utils/event.hpp>
#include <utils/function.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <tuple>
namespace clang { class FileEntry; }
namespace clang { class MacroArgs; }
namespace clang { class Module; }
namespace csabase { class Visitor; }

using namespace csabase;
using namespace clang::tooling;
using namespace clang;

// ----------------------------------------------------------------------------

static std::string const check_name("include-in-extern-c");

// ----------------------------------------------------------------------------

namespace
{

std::unordered_set<std::string> special{
    "bsl_stdhdrs_epilogue_recursive.h",
    "bsl_stdhdrs_epilogue.h",
    "bsl_stdhdrs_prologue.h",
};

struct data
    // Data attached to analyzer for this check.
{
    data();
        // Create an object of this type.

    std::map<SourceLocation, std::string>           d_includes;
    std::map<SourceRange, const LinkageSpecDecl *>  d_linkages;
    std::unordered_set<SourceLocation>              d_done;
    std::unordered_map<std::string, SourceLocation> d_prop;
};

data::data()
{
}

struct report
{
    report(Analyser& analyser,
           PPObserver::CallbackType type = PPObserver::e_None);
        // Create an object of this type, that will use the specified
        // 'analyser'.  Optionally specify a 'type' to identify the callback
        // that will be invoked, for preprocessor callbacks that have the same
        // signature.

    const LinkageSpecDecl *get_linkage(SourceLocation sl);

    const LinkageSpecDecl *get_local_linkage(SourceLocation sl);

    void operator()(SourceRange range);

    void operator()(SourceLocation   HashLoc,
                    const Token&     IncludeTok,
                    StringRef        FileName,
                    bool             IsAngled,
                    CharSourceRange  FilenameRange,
                    const FileEntry *File,
                    StringRef        SearchPath,
                    StringRef        RelativePath,
                    const Module    *Imported);

    void operator()(const FileEntry&           ParentFile,
                    const Token&               FilenameTok,
                    SrcMgr::CharacteristicKind FileType);

    void operator()(SourceLocation        Loc,
                    const Token&          MacroNameTok,
                    const MacroDirective *MD);

    void operator()(const Token&          MacroNameTok,
                    const MacroDirective *MD,
                    SourceRange           Range);

    void operator()(const LinkageSpecDecl *decl);

    void operator()();

    Analyser&                d_analyser;
    data&                    d_data;
    PPObserver::CallbackType d_type;

    SourceManager&           m;
};

report::report(Analyser& analyser, PPObserver::CallbackType type)
: d_analyser(analyser)
, d_data(analyser.attachment<data>())
, d_type(type)
, m(analyser.manager())
{
}

const LinkageSpecDecl *report::get_linkage(SourceLocation sl)
{
    const LinkageSpecDecl *result = 0;
    for (const auto& r : d_data.d_linkages) {
        if (!m.isBeforeInTranslationUnit(sl, r.first.getBegin()) &&
            !m.isBeforeInTranslationUnit(r.first.getEnd(), sl)) {
            result = r.second;
        }
    }
    return result;
}

const LinkageSpecDecl *report::get_local_linkage(SourceLocation sl)
{
    const LinkageSpecDecl *result = 0;
    for (const auto& r : d_data.d_linkages) {
        if (!m.isBeforeInTranslationUnit(sl, r.first.getBegin()) &&
            !m.isBeforeInTranslationUnit(r.first.getEnd(), sl) &&
            m.getFileID(m.getExpansionLoc(sl)) ==
            m.getFileID(m.getExpansionLoc(r.first.getBegin()))) {
            result = r.second;
        }
    }
    return result;
}

// InclusionDirective
void report::operator()(SourceLocation   HashLoc,
                        const Token&     IncludeTok,
                        StringRef        FileName,
                        bool             IsAngled,
                        CharSourceRange  FilenameRange,
                        const FileEntry *File,
                        StringRef        SearchPath,
                        StringRef        RelativePath,
                        const Module    *Imported)
{
    d_data.d_includes.insert({FilenameRange.getBegin(), FileName});
}

// FileSkipped
void report::operator()(const FileEntry&           ParentFile,
                        const Token&               FilenameTok,
                        SrcMgr::CharacteristicKind FileType)
{
    SourceLocation sl = m.getExpansionLoc(FilenameTok.getLocation());
    if (!special.count(llvm::sys::path::filename(m.getFilename(sl))))
    {
        llvm::StringRef file(
            FilenameTok.getLiteralData() + 1, FilenameTok.getLength() - 2);
        d_data.d_includes.insert({sl, file});
    }
}

// Comment
// SourceRangeSkipped
void report::operator()(SourceRange Range)
{
    SourceLocation sl = Range.getBegin();
    llvm::StringRef s = d_analyser.get_source(Range);
    llvm::StringRef f = llvm::sys::path::filename(m.getFilename(sl));
    llvm::SmallVector<llvm::StringRef, 7> matches;
    if (d_type == PPObserver::e_FileSkipped) {
        if (!special.count(f)) {
            size_t pos = 0;
            llvm::Regex r(" *ifn?def *INCLUDED_.*[[:space:]]+"
                          "# *include +[<\"]([^\">]*)[\">]");
            llvm::SmallVector<llvm::StringRef, 7> matches;
            if (r.match(s, &matches) && s.find(matches[0]) == 0) {
                d_data.d_includes.insert(
                    {sl.getLocWithOffset(s.find(matches[1])), matches[1]});
            }
        }
    } else {
        if (!d_data.d_prop.count(f)) {
            static std::string pattern;
            if (!pattern.size()) {
                pattern = "Copyright.* (";
                for (char c : d_analyser.config()->toplevel_namespace()) {
                    pattern = pattern + ".*" + c;
                }
                pattern += ").* ";
            }
            static llvm::Regex r(
                pattern, llvm::Regex::IgnoreCase | llvm::Regex::Newline);
            if (r.match(s, &matches)) {
                d_data.d_prop[f] = sl.getLocWithOffset(s.find(matches[1]));
            }
        }
    }
}

void report::operator()(const LinkageSpecDecl *decl)
{
    if (decl->hasBraces()) {
        d_data.d_linkages[decl->getSourceRange()] = decl;
    }
}

// TranslationUnitDone
void report::operator()()
{
    llvm::StringRef ns = d_analyser.config()->toplevel_namespace();
    for (const auto& f : d_data.d_includes) {
        SourceLocation sl = m.getExpansionLoc(f.first);
        const LinkageSpecDecl *lsd = get_local_linkage(sl);
        if (!lsd ||
            lsd->getLanguage() != LinkageSpecDecl::lang_c ||
            !d_data.d_prop.count(f.second)) {
            continue;
        }
        d_analyser.report(sl, check_name, "IC01",
                         "'%0' header included within C linkage specification",
                          true)
            << ns;
        d_analyser.report(lsd->getLocation(), check_name, "IC01",
                          "C linkage specification here",
                          true, DiagnosticIDs::Note);
        d_analyser.report(d_data.d_prop[f.second], check_name, "IC01",
                          "'%0' evidence here",
                          true, DiagnosticIDs::Note)
            << ns;
    }
}

void subscribe(Analyser& analyser, Visitor& visitor, PPObserver& observer)
    // Hook up the callback functions.
{
    observer.onComment              += report(analyser);
    observer.onPPInclusionDirective += report(analyser,
                                                observer.e_InclusionDirective);
    observer.onPPFileSkipped        += report(analyser,
                                                observer.e_FileSkipped);
    observer.onPPSourceRangeSkipped += report(analyser,
                                                observer.e_SourceRangeSkipped);
    analyser.onTranslationUnitDone  += report(analyser);
    visitor.onLinkageSpecDecl       += report(analyser);
}

}  // close anonymous namespace

// ----------------------------------------------------------------------------

static RegisterCheck c1(check_name, &subscribe);

// ----------------------------------------------------------------------------
// Copyright (C) 2014 Bloomberg Finance L.P.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------- END-OF-FILE ----------------------------------
