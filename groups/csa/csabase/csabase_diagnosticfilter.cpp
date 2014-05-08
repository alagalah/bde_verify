// csabase_diagnosticfilter.cpp                                       -*-C++-*-
// ----------------------------------------------------------------------------
// Copyright 2012 Dietmar Kuehl http://www.dietmar-kuehl.de              
// Distributed under the Boost Software License, Version 1.0. (See file  
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt).     
// ----------------------------------------------------------------------------

#include <csabase_diagnosticfilter.h>
#include <csabase_analyser.h>
#include <csabase_debug.h>
#include <csabase_registercheck.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/LexDiagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/FileManager.h>
#include <llvm/Support/raw_ostream.h>
#ident "$Id$"

// ----------------------------------------------------------------------------

static std::string const check_name("diagnostic-filter");

// ----------------------------------------------------------------------------

namespace CB = csabase;

// ----------------------------------------------------------------------------

CB::DiagnosticFilter::DiagnosticFilter(CB::Analyser const& analyser,
                                       bool toplevel_only,
                                       clang::DiagnosticOptions & options)
    : d_options(&options)
    , d_client(new clang::TextDiagnosticPrinter(llvm::errs(), d_options))
    , d_analyser(&analyser)
    , d_toplevel_only(toplevel_only)
{
}

CB::DiagnosticFilter::~DiagnosticFilter()
{
}

// ----------------------------------------------------------------------------

void
CB::DiagnosticFilter::BeginSourceFile(clang::LangOptions const&  opts,
                                      clang::Preprocessor const* pp)
{
    d_client->BeginSourceFile(opts, pp);
}

void
CB::DiagnosticFilter::EndSourceFile()
{
    d_client->EndSourceFile();
}

bool
CB::DiagnosticFilter::IncludeInDiagnosticCount() const
{
    return true;
}

static std::string
get_filename(clang::Diagnostic const& d)
{
    clang::SourceManager& m = d.getSourceManager();
    std::string name;
    if (const clang::FileEntry *f =
            m.getFileEntryForID(m.getFileID(d.getLocation()))) {
        name = f->getName();
    }
    return name;
}

void
CB::DiagnosticFilter::HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                                       clang::Diagnostic const&        info)
{
    if (   clang::DiagnosticsEngine::Warning < level
        || (   !info.getLocation().isFileID()
            && info.getID() != clang::diag::pp_pragma_once_in_main_file
           )
        || (   d_analyser->is_component(get_filename(info))
            && !d_analyser->is_generated(info.getLocation())
            && (   !d_toplevel_only
                || d_analyser->manager().getMainFileID() ==
                   d_analyser->manager().getFileID(info.getLocation())
               )
           )
       )
    {
        DiagnosticConsumer::HandleDiagnostic(level, info);
        d_client->HandleDiagnostic(level, info);
    }
}

clang::DiagnosticConsumer*
CB::DiagnosticFilter::clone(clang::DiagnosticsEngine&) const
{
    return new CB::DiagnosticFilter(*d_analyser, d_toplevel_only, *d_options);
}

// ----------------------------------------------------------------------------

static void check(CB::Analyser& analyser, const clang::TranslationUnitDecl*)
{
}

// ----------------------------------------------------------------------------

static CB::RegisterCheck register_check(check_name, &check);
