/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://www.qtsoftware.com/contact.
**
**************************************************************************/

#include "cppfindreferences.h"
#include "cppmodelmanagerinterface.h"
#include "cpptoolsconstants.h"

#include <texteditor/basetexteditor.h>
#include <find/searchresultwindow.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/filesearch.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/icore.h>

#include <ASTVisitor.h>
#include <AST.h>
#include <Control.h>
#include <Literals.h>
#include <TranslationUnit.h>
#include <Symbols.h>
#include <Names.h>
#include <Scope.h>

#include <cplusplus/CppDocument.h>
#include <cplusplus/CppBindings.h>
#include <cplusplus/ExpressionUnderCursor.h>
#include <cplusplus/ResolveExpression.h>
#include <cplusplus/Overview.h>
#include <cplusplus/TypeOfExpression.h>
#include <cplusplus/FastPreprocessor.h>

#include <QtCore/QTime>
#include <QtCore/QtConcurrentRun>
#include <QtCore/QDir>
#include <QtGui/QApplication>
#include <qtconcurrent/runextensions.h>

using namespace CppTools::Internal;
using namespace CPlusPlus;

namespace {

struct Process: protected ASTVisitor
{
public:
    Process(Document::Ptr doc, const Snapshot &snapshot,
            QFutureInterface<Utils::FileSearchResult> *future)
            : ASTVisitor(doc->control()),
              _future(future),
              _doc(doc),
              _snapshot(snapshot),
              _source(_doc->source()),
              _sem(doc->control())
    {
        _snapshot.insert(_doc);
    }

    void setGlobalNamespaceBinding(NamespaceBindingPtr globalNamespaceBinding)
    {
        _globalNamespaceBinding = globalNamespaceBinding;
    }

    QList<int> operator()(Symbol *symbol, Identifier *id, AST *ast)
    {
        _references.clear();
        _declSymbol = symbol;
        _id = id;
        _exprDoc = Document::create("<references>");
        accept(ast);
        return _references;
    }

protected:
    using ASTVisitor::visit;

    QString matchingLine(const Token &tk) const
    {
        const char *beg = _source.constData();
        const char *cp = beg + tk.offset;
        for (; cp != beg - 1; --cp) {
            if (*cp == '\n')
                break;
        }

        ++cp;

        const char *lineEnd = cp + 1;
        for (; *lineEnd; ++lineEnd) {
            if (*lineEnd == '\n')
                break;
        }

        const QString matchingLine = QString::fromUtf8(cp, lineEnd - cp);
        return matchingLine;

    }

    void reportResult(unsigned tokenIndex, const QList<Symbol *> &candidates)
    {
        const bool isStrongResult = checkCandidates(candidates);

        if (isStrongResult)
            reportResult(tokenIndex);
    }

    void reportResult(unsigned tokenIndex)
    {
        const Token &tk = tokenAt(tokenIndex);
        const QString lineText = matchingLine(tk);

        unsigned line, col;
        getTokenStartPosition(tokenIndex, &line, &col);

        if (col)
            --col;  // adjust the column position.

        const int len = tk.f.length;

        if (_future)
            _future->reportResult(Utils::FileSearchResult(QDir::toNativeSeparators(_doc->fileName()),
                                                          line, lineText, col, len));

        _references.append(tokenIndex);
    }

    bool checkCandidates(const QList<Symbol *> &candidates) const
    {
        if (Symbol *canonicalSymbol = LookupContext::canonicalSymbol(candidates, _globalNamespaceBinding.data())) {

#if 0
            qDebug() << "*** canonical symbol:" << canonicalSymbol->fileName()
                    << canonicalSymbol->line() << canonicalSymbol->column()
                    << "candidates:" << candidates.size();
#endif

            return isDeclSymbol(canonicalSymbol);
        }

        return false;
    }

    bool checkScope(Symbol *symbol, Symbol *otherSymbol) const
    {
        if (! (symbol && otherSymbol))
            return false;

        else if (symbol->scope() == otherSymbol->scope())
            return true;

        else if (symbol->name() && otherSymbol->name()) {

            if (! symbol->name()->isEqualTo(otherSymbol->name()))
                return false;

        } else if (symbol->name() != otherSymbol->name()) {
            return false;
        }

        return checkScope(symbol->enclosingSymbol(), otherSymbol->enclosingSymbol());
    }

    bool isDeclSymbol(Symbol *symbol) const
    {
        if (! symbol) {
            return false;

        } else if (symbol == _declSymbol) {
            return true;

        } else if (symbol->line() == _declSymbol->line() && symbol->column() == _declSymbol->column()) {
            if (! qstrcmp(symbol->fileName(), _declSymbol->fileName()))
                return true;

        } else if (symbol->isForwardClassDeclaration() && (_declSymbol->isClass() ||
                                                           _declSymbol->isForwardClassDeclaration())) {
            return checkScope(symbol, _declSymbol);

        } else if (_declSymbol->isForwardClassDeclaration() && (symbol->isClass() ||
                                                                symbol->isForwardClassDeclaration())) {
            return checkScope(symbol, _declSymbol);
        }

        return false;
    }

    LookupContext _previousContext;

    LookupContext currentContext(AST *ast)
    {
        unsigned line, column;
        getTokenStartPosition(ast->firstToken(), &line, &column);
        Symbol *lastVisibleSymbol = _doc->findSymbolAt(line, column);

        if (lastVisibleSymbol && lastVisibleSymbol == _previousContext.symbol())
            return _previousContext;

        LookupContext ctx(lastVisibleSymbol, _exprDoc, _doc, _snapshot);
        _previousContext = ctx;
        return ctx;
    }

    void ensureNameIsValid(NameAST *ast)
    {
        if (ast && ! ast->name)
            ast->name = _sem.check(ast, /*scope = */ 0);
    }

    virtual bool visit(MemInitializerAST *ast)
    {
        if (ast->name && ast->name->asSimpleName() != 0) {
            ensureNameIsValid(ast->name);

            SimpleNameAST *simple = ast->name->asSimpleName();
            if (identifier(simple->identifier_token) == _id) {
                LookupContext context = currentContext(ast);
                const QList<Symbol *> candidates = context.resolve(simple->name);
                reportResult(simple->identifier_token, candidates);
            }
        }
        accept(ast->expression);
        return false;
    }

    virtual bool visit(PostfixExpressionAST *ast)
    {
        _postfixExpressionStack.append(ast);
        return true;
    }

    virtual void endVisit(PostfixExpressionAST *)
    {
        _postfixExpressionStack.removeLast();
    }

    virtual bool visit(MemberAccessAST *ast)
    {
        if (ast->member_name) {
            if (SimpleNameAST *simple = ast->member_name->asSimpleName()) {
                if (identifier(simple->identifier_token) == _id) {
                    Q_ASSERT(! _postfixExpressionStack.isEmpty());

                    checkExpression(_postfixExpressionStack.last()->firstToken(),
                                    simple->identifier_token);

                    return false;
                }
            }
        }

        return true;
    }

    void checkExpression(unsigned startToken, unsigned endToken)
    {
        const unsigned begin = tokenAt(startToken).begin();
        const unsigned end = tokenAt(endToken).end();

        const QString expression = _source.mid(begin, end - begin);
        // qDebug() << "*** check expression:" << expression;

        TypeOfExpression typeofExpression;
        typeofExpression.setSnapshot(_snapshot);

        unsigned line, column;
        getTokenStartPosition(startToken, &line, &column);
        Symbol *lastVisibleSymbol = _doc->findSymbolAt(line, column);

        const QList<TypeOfExpression::Result> results =
                typeofExpression(expression, _doc, lastVisibleSymbol,
                                 TypeOfExpression::Preprocess);

        QList<Symbol *> candidates;

        foreach (TypeOfExpression::Result r, results) {
            FullySpecifiedType ty = r.first;
            Symbol *lastVisibleSymbol = r.second;

            candidates.append(lastVisibleSymbol);
        }

        reportResult(endToken, candidates);
    }

    virtual bool visit(QualifiedNameAST *ast)
    {
        for (NestedNameSpecifierAST *nested_name_specifier = ast->nested_name_specifier;
             nested_name_specifier; nested_name_specifier = nested_name_specifier->next) {

            if (NameAST *class_or_namespace_name = nested_name_specifier->class_or_namespace_name) {
                SimpleNameAST *simple_name = class_or_namespace_name->asSimpleName();

                TemplateIdAST *template_id = 0;
                if (! simple_name) {
                    template_id = class_or_namespace_name->asTemplateId();

                    if (template_id) {
                        for (TemplateArgumentListAST *template_arguments = template_id->template_arguments;
                             template_arguments; template_arguments = template_arguments->next) {
                            accept(template_arguments->template_argument);
                        }
                    }
                }

                if (simple_name || template_id) {
                    const unsigned identifier_token = simple_name
                               ? simple_name->identifier_token
                               : template_id->identifier_token;

                    if (identifier(identifier_token) == _id)
                        checkExpression(ast->firstToken(), identifier_token);
                }
            }
        }

        if (NameAST *unqualified_name = ast->unqualified_name) {
            unsigned identifier_token = 0;

            if (SimpleNameAST *simple_name = unqualified_name->asSimpleName())
                identifier_token = simple_name->identifier_token;

            else if (DestructorNameAST *dtor_name = unqualified_name->asDestructorName())
                identifier_token = dtor_name->identifier_token;

            TemplateIdAST *template_id = 0;
            if (! identifier_token) {
                template_id = unqualified_name->asTemplateId();

                if (template_id) {
                    identifier_token = template_id->identifier_token;

                    for (TemplateArgumentListAST *template_arguments = template_id->template_arguments;
                         template_arguments; template_arguments = template_arguments->next) {
                        accept(template_arguments->template_argument);
                    }
                }
            }

            if (identifier_token && identifier(identifier_token) == _id)
                checkExpression(ast->firstToken(), identifier_token);
        }

        return false;
    }

    virtual bool visit(EnumeratorAST *ast)
    {
        Identifier *id = identifier(ast->identifier_token);
        if (id == _id) {
            LookupContext context = currentContext(ast);
            const QList<Symbol *> candidates = context.resolve(control()->nameId(id));
            reportResult(ast->identifier_token, candidates);
        }

        accept(ast->expression);

        return false;
    }

    virtual bool visit(SimpleNameAST *ast)
    {
        Identifier *id = identifier(ast->identifier_token);
        if (id == _id) {
            LookupContext context = currentContext(ast);
            const QList<Symbol *> candidates = context.resolve(ast->name);
            reportResult(ast->identifier_token, candidates);
        }

        return false;
    }

    virtual bool visit(DestructorNameAST *ast)
    {
        Identifier *id = identifier(ast->identifier_token);
        if (id == _id) {
            LookupContext context = currentContext(ast);
            const QList<Symbol *> candidates = context.resolve(ast->name);
            reportResult(ast->identifier_token, candidates);
        }

        return false;
    }

    virtual bool visit(TemplateIdAST *ast)
    {
        if (_id == identifier(ast->identifier_token)) {
            LookupContext context = currentContext(ast);
            const QList<Symbol *> candidates = context.resolve(ast->name);
            reportResult(ast->identifier_token, candidates);
        }

        for (TemplateArgumentListAST *template_arguments = ast->template_arguments;
             template_arguments; template_arguments = template_arguments->next) {
            accept(template_arguments->template_argument);
        }

        return false;
    }

    virtual bool visit(ParameterDeclarationAST *ast)
    {
        for (SpecifierAST *spec = ast->type_specifier; spec; spec = spec->next)
            accept(spec);

        if (DeclaratorAST *declarator = ast->declarator) {
            for (SpecifierAST *attr = declarator->attributes; attr; attr = attr->next)
                accept(attr);

            for (PtrOperatorAST *ptr_op = declarator->ptr_operators; ptr_op; ptr_op = ptr_op->next)
                accept(ptr_op);

            // ### TODO: well, not exactly. We need to look at qualified-name-ids and nested-declarators.
            // accept(declarator->core_declarator);

            for (PostfixDeclaratorAST *fx_op = declarator->postfix_declarators; fx_op; fx_op = fx_op->next)
                accept(fx_op);

            for (SpecifierAST *spec = declarator->post_attributes; spec; spec = spec->next)
                accept(spec);

            accept(declarator->initializer);
        }

        accept(ast->expression);
        return false;
    }

    virtual bool visit(ExpressionOrDeclarationStatementAST *ast)
    {
        accept(ast->declaration);
        return false;
    }

    virtual bool visit(FunctionDeclaratorAST *ast)
    {
        accept(ast->parameters);

        for (SpecifierAST *spec = ast->cv_qualifier_seq; spec; spec = spec->next)
            accept(spec);

        accept(ast->exception_specification);

        return false;
    }

private:
    QFutureInterface<Utils::FileSearchResult> *_future;
    Identifier *_id; // ### remove me
    Symbol *_declSymbol;
    Document::Ptr _doc;
    Snapshot _snapshot;
    QByteArray _source;
    Document::Ptr _exprDoc;
    Semantic _sem;
    NamespaceBindingPtr _globalNamespaceBinding;
    QList<PostfixExpressionAST *> _postfixExpressionStack;
    QList<QualifiedNameAST *> _qualifiedNameStack;
    QList<int> _references;
};

} // end of anonymous namespace

CppFindReferences::CppFindReferences(CppTools::CppModelManagerInterface *modelManager)
    : _modelManager(modelManager),
      _resultWindow(ExtensionSystem::PluginManager::instance()->getObject<Find::SearchResultWindow>())
{
    m_watcher.setPendingResultsLimit(1);
    connect(&m_watcher, SIGNAL(resultReadyAt(int)), this, SLOT(displayResult(int)));
    connect(&m_watcher, SIGNAL(finished()), this, SLOT(searchFinished()));
}

CppFindReferences::~CppFindReferences()
{
}

QList<int> CppFindReferences::references(Symbol *symbol,
                                         Document::Ptr doc,
                                         const Snapshot& snapshot) const
{
    Identifier *id = 0;
    if (Identifier *symbolId = symbol->identifier())
        id = doc->control()->findIdentifier(symbolId->chars(), symbolId->size());

    QList<int> references;

    if (! id)
        return references;

    TranslationUnit *translationUnit = doc->translationUnit();
    Q_ASSERT(translationUnit != 0);

    Process process(doc, snapshot, /*future = */ 0);
    process.setGlobalNamespaceBinding(bind(doc, snapshot));
    references = process(symbol, id, translationUnit->ast());

    return references;
}

static void find_helper(QFutureInterface<Utils::FileSearchResult> &future,
                        const QMap<QString, QString> wl,
                        Snapshot snapshot,
                        Symbol *symbol)
{
    QTime tm;
    tm.start();

    Identifier *symbolId = symbol->identifier();
    Q_ASSERT(symbolId != 0);

    const QString sourceFile = QString::fromUtf8(symbol->fileName(), symbol->fileNameLength());

    QStringList files(sourceFile);

    if (symbol->isClass() || symbol->isForwardClassDeclaration()) {
        foreach (const Document::Ptr &doc, snapshot) {
            if (doc->fileName() == sourceFile)
                continue;

            Control *control = doc->control();

            if (control->findIdentifier(symbolId->chars(), symbolId->size()))
                files.append(doc->fileName());
        }
    } else {
        files += snapshot.dependsOn(sourceFile);
    }

    //qDebug() << "done in:" << tm.elapsed() << "number of files to parse:" << files.size();

    future.setProgressRange(0, files.size());

    for (int i = 0; i < files.size(); ++i) {
        if (future.isPaused())
            future.waitForResume();

        if (future.isCanceled())
            break;

        const QString &fileName = files.at(i);
        future.setProgressValueAndText(i, QFileInfo(fileName).fileName());

        if (Document::Ptr previousDoc = snapshot.value(fileName)) {
            Control *control = previousDoc->control();
            Identifier *id = control->findIdentifier(symbolId->chars(), symbolId->size());
            if (! id)
                continue; // skip this document, it's not using symbolId.
        }

        QByteArray source;

        if (wl.contains(fileName))
            source = snapshot.preprocessedCode(wl.value(fileName), fileName);
        else {
            QFile file(fileName);
            if (! file.open(QFile::ReadOnly))
                continue;

            const QString contents = QTextStream(&file).readAll(); // ### FIXME
            source = snapshot.preprocessedCode(contents, fileName);
        }

        Document::Ptr doc = snapshot.documentFromSource(source, fileName);
        doc->tokenize();

        Control *control = doc->control();
        if (Identifier *id = control->findIdentifier(symbolId->chars(), symbolId->size())) {
            QTime tm;
            tm.start();
            doc->parse();

            //qDebug() << "***" << unit->fileName() << "parsed in:" << tm.elapsed();

            tm.start();
            doc->check();
            //qDebug() << "***" << unit->fileName() << "checked in:" << tm.elapsed();

            tm.start();

            Process process(doc, snapshot, &future);
            process.setGlobalNamespaceBinding(bind(doc, snapshot));

            TranslationUnit *unit = doc->translationUnit();
            process(symbol, id, unit->ast());

            //qDebug() << "***" << unit->fileName() << "processed in:" << tm.elapsed();
        }
    }

    future.setProgressValue(files.size());
}

void CppFindReferences::findUsages(Symbol *symbol)
{
    Find::SearchResult *search = _resultWindow->startNewSearch(Find::SearchResultWindow::SearchOnly);

    connect(search, SIGNAL(activated(Find::SearchResultItem)),
            this, SLOT(openEditor(Find::SearchResultItem)));

    findAll_helper(symbol);
}

void CppFindReferences::renameUsages(Symbol *symbol)
{
    if (Identifier *id = symbol->identifier()) {
        const QString textToReplace = QString::fromUtf8(id->chars(), id->size());

        Find::SearchResult *search = _resultWindow->startNewSearch(Find::SearchResultWindow::SearchAndReplace);
        _resultWindow->setTextToReplace(textToReplace);

        connect(search, SIGNAL(activated(Find::SearchResultItem)),
                this, SLOT(openEditor(Find::SearchResultItem)));

        connect(search, SIGNAL(replaceButtonClicked(QString,QList<Find::SearchResultItem>)),
                SLOT(onReplaceButtonClicked(QString,QList<Find::SearchResultItem>)));

        findAll_helper(symbol);
    }
}

void CppFindReferences::findAll_helper(Symbol *symbol)
{
    _resultWindow->popup(true);

    const Snapshot snapshot = _modelManager->snapshot();
    const QMap<QString, QString> wl = _modelManager->workingCopy();

    Core::ProgressManager *progressManager = Core::ICore::instance()->progressManager();

    QFuture<Utils::FileSearchResult> result = QtConcurrent::run(&find_helper, wl, snapshot, symbol);
    m_watcher.setFuture(result);

    Core::FutureProgress *progress = progressManager->addTask(result, tr("Searching..."),
                                                              CppTools::Constants::TASK_SEARCH,
                                                              Core::ProgressManager::CloseOnSuccess);

    connect(progress, SIGNAL(clicked()), _resultWindow, SLOT(popup()));
}

static void applyChanges(QTextDocument *doc, const QString &text, const QList<Find::SearchResultItem> &items)
{
    QList<QTextCursor> cursors;

    foreach (const Find::SearchResultItem &item, items) {
        const int blockNumber = item.lineNumber - 1;
        QTextCursor tc(doc->findBlockByNumber(blockNumber));
        tc.setPosition(tc.position() + item.searchTermStart);
        tc.setPosition(tc.position() + item.searchTermLength,
                       QTextCursor::KeepAnchor);
        cursors.append(tc);
    }

    foreach (QTextCursor tc, cursors)
        tc.insertText(text);
}

void CppFindReferences::onReplaceButtonClicked(const QString &text,
                                               const QList<Find::SearchResultItem> &items)
{
    if (text.isEmpty())
        return;

    QHash<QString, QList<Find::SearchResultItem> > changes;

    foreach (const Find::SearchResultItem &item, items)
        changes[item.fileName].append(item);

    Core::EditorManager *editorManager = Core::EditorManager::instance();

    QHashIterator<QString, QList<Find::SearchResultItem> > it(changes);
    while (it.hasNext()) {
        it.next();

        const QString fileName = it.key();
        const QList<Find::SearchResultItem> items = it.value();

        const QList<Core::IEditor *> editors = editorManager->editorsForFileName(fileName);
        TextEditor::BaseTextEditor *textEditor = 0;
        foreach (Core::IEditor *editor, editors) {
            textEditor = qobject_cast<TextEditor::BaseTextEditor *>(editor->widget());
            if (textEditor != 0)
                break;
        }

        if (textEditor != 0) {
            QTextCursor tc = textEditor->textCursor();
            tc.beginEditBlock();
            applyChanges(textEditor->document(), text, items);
            tc.endEditBlock();
        } else {
            QFile file(fileName);

            if (file.open(QFile::ReadOnly)) {
                QTextStream stream(&file);
                // ### set the encoding
                const QString plainText = stream.readAll();
                file.close();

                QTextDocument doc;
                doc.setPlainText(plainText);

                applyChanges(&doc, text, items);

                QFile newFile(fileName);
                if (newFile.open(QFile::WriteOnly)) {
                    QTextStream stream(&newFile);
                    // ### set the encoding
                    stream << doc.toPlainText();
                }
            }
        }
    }

    const QStringList fileNames = changes.keys();
    _modelManager->updateSourceFiles(fileNames);
    _resultWindow->hide();
}

void CppFindReferences::displayResult(int index)
{
    Utils::FileSearchResult result = m_watcher.future().resultAt(index);
    _resultWindow->addResult(result.fileName,
                             result.lineNumber,
                             result.matchingLine,
                             result.matchStart,
                             result.matchLength);
}

void CppFindReferences::searchFinished()
{
    emit changed();
}

void CppFindReferences::openEditor(const Find::SearchResultItem &item)
{
    TextEditor::BaseTextEditor::openEditorAt(item.fileName, item.lineNumber, item.searchTermStart);
}

