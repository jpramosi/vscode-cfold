// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "indexer.hh"

#include "clang_tu.hh"
#include "log.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "sema_manager.hh"

#include <clang/AST/AST.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <clang/Index/IndexDataConsumer.h>
#include <clang/Index/IndexingAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/CrashRecoveryContext.h>
#include <llvm/Support/Path.h>

#include <algorithm>
#include <inttypes.h>
#include <map>
#include <unordered_set>

using namespace clang;

namespace ccls { @_0_
namespace { @_1_

GroupMatch *multiVersionMatcher;

struct File { @_2_
  std::string path;
  int64_t mtime;
  std::string content;
  std::unique_ptr<IndexFile> db;
}; @_2_

struct IndexParam { @_3_
  std::unordered_map<FileID, File> uid2file;
  std::unordered_map<FileID, bool> uid2multi;
  struct DeclInfo { @_4_
    Usr usr;
    std::string short_name;
    std::string qualified;
  }; @_4_
  std::unordered_map<const Decl *, DeclInfo> decl2Info;

  VFS &vfs;
  ASTContext *ctx;
  bool no_linkage;
  IndexParam(VFS &vfs, bool no_linkage) : vfs(vfs), no_linkage(no_linkage) {}

  void seenFile(FileID fid) { @_5_
    // If this is the first time we have seen the file (ignoring if we are
    // generating an index for it):
    auto [it, inserted] = uid2file.try_emplace(fid);
    if (inserted) { @_6_
      const FileEntry *fe = ctx->getSourceManager().getFileEntryForID(fid);
      if (!fe)
        return;
      std::string path = pathFromFileEntry(*fe);
      it->second.path = path;
      it->second.mtime = fe->getModificationTime();
      if (!it->second.mtime)
        if (auto tim = lastWriteTime(path))
          it->second.mtime = *tim;
      if (std::optional<std::string> content = readContent(path))
        it->second.content = *content;

      if (!vfs.stamp(path, it->second.mtime, no_linkage ? 3 : 1))
        return;
      it->second.db =
          std::make_unique<IndexFile>(path, it->second.content, no_linkage);
    } @_6_
  } @_5_

  IndexFile *consumeFile(FileID fid) { @_7_
    seenFile(fid);
    return uid2file[fid].db.get();
  } @_7_

  bool useMultiVersion(FileID fid) { @_8_
    auto it = uid2multi.try_emplace(fid);
    if (it.second)
      if (const FileEntry *fe = ctx->getSourceManager().getFileEntryForID(fid))
        it.first->second = multiVersionMatcher->matches(pathFromFileEntry(*fe));
    return it.first->second;
  } @_8_
}; @_3_

StringRef getSourceInRange(const SourceManager &sm, const LangOptions &langOpts,
                           SourceRange sr) { @_9_
  SourceLocation bloc = sr.getBegin(), eLoc = sr.getEnd();
  std::pair<FileID, unsigned> bInfo = sm.getDecomposedLoc(bloc),
                              eInfo = sm.getDecomposedLoc(eLoc);
  bool invalid = false;
  StringRef buf = sm.getBufferData(bInfo.first, &invalid);
  if (invalid)
    return "";
  return buf.substr(bInfo.second,
                    eInfo.second +
                        Lexer::MeasureTokenLength(eLoc, sm, langOpts) -
                        bInfo.second);
} @_9_

Kind getKind(const Decl *d, SymbolKind &kind) { @_10_
  switch (d->getKind()) { @_11_
  case Decl::LinkageSpec:
    return Kind::Invalid;
  case Decl::Namespace:
  case Decl::NamespaceAlias:
    kind = SymbolKind::Namespace;
    return Kind::Type;
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCImplementation:
  case Decl::ObjCInterface:
  case Decl::ObjCProtocol:
    kind = SymbolKind::Interface;
    return Kind::Type;
  case Decl::ObjCMethod:
    kind = SymbolKind::Method;
    return Kind::Func;
  case Decl::ObjCProperty:
    kind = SymbolKind::Property;
    return Kind::Type;
  case Decl::ClassTemplate:
    kind = SymbolKind::Class;
    return Kind::Type;
  case Decl::FunctionTemplate:
    kind = SymbolKind::Function;
    return Kind::Func;
  case Decl::TypeAliasTemplate:
    kind = SymbolKind::TypeAlias;
    return Kind::Type;
  case Decl::VarTemplate:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::TemplateTemplateParm:
    kind = SymbolKind::TypeParameter;
    return Kind::Type;
  case Decl::Enum:
    kind = SymbolKind::Enum;
    return Kind::Type;
  case Decl::CXXRecord:
  case Decl::Record:
    kind = SymbolKind::Class;
    // spec has no Union, use Class
    if (auto *rd = dyn_cast<RecordDecl>(d))
      if (rd->getTagKind() == TTK_Struct)
        kind = SymbolKind::Struct;
    return Kind::Type;
  case Decl::ClassTemplateSpecialization:
  case Decl::ClassTemplatePartialSpecialization:
    kind = SymbolKind::Class;
    return Kind::Type;
  case Decl::TemplateTypeParm:
    kind = SymbolKind::TypeParameter;
    return Kind::Type;
  case Decl::TypeAlias:
  case Decl::Typedef:
  case Decl::UnresolvedUsingTypename:
    kind = SymbolKind::TypeAlias;
    return Kind::Type;
  case Decl::Using:
    kind = SymbolKind::Null; // ignored
    return Kind::Invalid;
  case Decl::Binding:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::Field:
  case Decl::ObjCIvar:
    kind = SymbolKind::Field;
    return Kind::Var;
  case Decl::Function:
    kind = SymbolKind::Function;
    return Kind::Func;
  case Decl::CXXMethod: { @_12_
    const auto *md = cast<CXXMethodDecl>(d);
    kind = md->isStatic() ? SymbolKind::StaticMethod : SymbolKind::Method;
    return Kind::Func;
  } @_12_
  case Decl::CXXConstructor:
    kind = SymbolKind::Constructor;
    return Kind::Func;
  case Decl::CXXConversion:
  case Decl::CXXDestructor:
    kind = SymbolKind::Method;
    return Kind::Func;
  case Decl::NonTypeTemplateParm:
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var;
  case Decl::Var:
  case Decl::Decomposition:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::ImplicitParam:
  case Decl::ParmVar:
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var;
  case Decl::VarTemplateSpecialization:
  case Decl::VarTemplatePartialSpecialization:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::EnumConstant:
    kind = SymbolKind::EnumMember;
    return Kind::Var;
  case Decl::UnresolvedUsingValue:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::TranslationUnit:
    return Kind::Invalid;

  default:
    return Kind::Invalid;
  } @_11_
} @_10_

LanguageId getDeclLanguage(const Decl *d) { @_13_
  switch (d->getKind()) { @_14_
  default:
    return LanguageId::C;
  case Decl::ImplicitParam:
  case Decl::ObjCAtDefsField:
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCCompatibleAlias:
  case Decl::ObjCImplementation:
  case Decl::ObjCInterface:
  case Decl::ObjCIvar:
  case Decl::ObjCMethod:
  case Decl::ObjCProperty:
  case Decl::ObjCPropertyImpl:
  case Decl::ObjCProtocol:
  case Decl::ObjCTypeParam:
    return LanguageId::ObjC;
  case Decl::CXXConstructor:
  case Decl::CXXConversion:
  case Decl::CXXDestructor:
  case Decl::CXXMethod:
  case Decl::CXXRecord:
  case Decl::ClassTemplate:
  case Decl::ClassTemplatePartialSpecialization:
  case Decl::ClassTemplateSpecialization:
  case Decl::Friend:
  case Decl::FriendTemplate:
  case Decl::FunctionTemplate:
  case Decl::LinkageSpec:
  case Decl::Namespace:
  case Decl::NamespaceAlias:
  case Decl::NonTypeTemplateParm:
  case Decl::StaticAssert:
  case Decl::TemplateTemplateParm:
  case Decl::TemplateTypeParm:
  case Decl::UnresolvedUsingTypename:
  case Decl::UnresolvedUsingValue:
  case Decl::Using:
  case Decl::UsingDirective:
  case Decl::UsingShadow:
    return LanguageId::Cpp;
  } @_14_
} @_13_

// clang/lib/AST/DeclPrinter.cpp
QualType getBaseType(QualType t, bool deduce_auto) { @_15_
  QualType baseType = t;
  while (!baseType.isNull() && !baseType->isSpecifierType()) { @_16_
    if (const PointerType *pTy = baseType->getAs<PointerType>())
      baseType = pTy->getPointeeType();
    else if (const BlockPointerType *bPy = baseType->getAs<BlockPointerType>())
      baseType = bPy->getPointeeType();
    else if (const ArrayType *aTy = dyn_cast<ArrayType>(baseType))
      baseType = aTy->getElementType();
    else if (const VectorType *vTy = baseType->getAs<VectorType>())
      baseType = vTy->getElementType();
    else if (const ReferenceType *rTy = baseType->getAs<ReferenceType>())
      baseType = rTy->getPointeeType();
    else if (const ParenType *pTy = baseType->getAs<ParenType>())
      baseType = pTy->desugar();
    else if (deduce_auto) { @_17_
      if (const AutoType *aTy = baseType->getAs<AutoType>())
        baseType = aTy->getDeducedType();
      else
        break;
    } else @_17_
      break;
  } @_16_
  return baseType;
} @_15_

const Decl *getTypeDecl(QualType t, bool *specialization = nullptr) { @_18_
  Decl *d = nullptr;
  t = getBaseType(t.getUnqualifiedType(), true);
  const Type *tp = t.getTypePtrOrNull();
  if (!tp)
    return nullptr;

try_again:
  switch (tp->getTypeClass()) { @_19_
  case Type::Typedef:
    d = cast<TypedefType>(tp)->getDecl();
    break;
  case Type::ObjCObject:
    d = cast<ObjCObjectType>(tp)->getInterface();
    break;
  case Type::ObjCInterface:
    d = cast<ObjCInterfaceType>(tp)->getDecl();
    break;
  case Type::Record:
  case Type::Enum:
    d = cast<TagType>(tp)->getDecl();
    break;
  case Type::TemplateTypeParm:
    d = cast<TemplateTypeParmType>(tp)->getDecl();
    break;
  case Type::TemplateSpecialization:
    if (specialization)
      *specialization = true;
    if (const RecordType *record = tp->getAs<RecordType>())
      d = record->getDecl();
    else
      d = cast<TemplateSpecializationType>(tp)
              ->getTemplateName()
              .getAsTemplateDecl();
    break;

  case Type::Auto:
  case Type::DeducedTemplateSpecialization:
    tp = cast<DeducedType>(tp)->getDeducedType().getTypePtrOrNull();
    if (tp)
      goto try_again;
    break;

  case Type::InjectedClassName:
    d = cast<InjectedClassNameType>(tp)->getDecl();
    break;

    // FIXME: Template type parameters!

  case Type::Elaborated:
    tp = cast<ElaboratedType>(tp)->getNamedType().getTypePtrOrNull();
    goto try_again;

  default:
    break;
  } @_19_
  return d;
} @_18_

const Decl *getAdjustedDecl(const Decl *d) { @_20_
  while (d) { @_21_
    if (auto *r = dyn_cast<CXXRecordDecl>(d)) { @_22_
      if (auto *s = dyn_cast<ClassTemplateSpecializationDecl>(r)) { @_23_
        if (!s->isExplicitSpecialization()) { @_24_
          llvm::PointerUnion<ClassTemplateDecl *,
                             ClassTemplatePartialSpecializationDecl *>
              result = s->getSpecializedTemplateOrPartial();
          if (result.is<ClassTemplateDecl *>())
            d = result.get<ClassTemplateDecl *>();
          else
            d = result.get<ClassTemplatePartialSpecializationDecl *>();
          continue;
        } @_24_
      } else if (auto *d1 = r->getInstantiatedFromMemberClass()) {
        d = d1;
        continue;
      } @_23_
    } else if (auto *ed = dyn_cast<EnumDecl>(d)) {
      if (auto *d1 = ed->getInstantiatedFromMemberEnum()) { @_25_
        d = d1;
        continue;
      } @_25_
    } @_22_
    break;
  } @_21_
  return d;
} @_20_

bool validateRecord(const RecordDecl *rd) { @_26_
  for (const auto *i : rd->fields()) { @_27_
    QualType fqt = i->getType();
    if (fqt->isIncompleteType() || fqt->isDependentType())
      return false;
    if (const RecordType *childType = i->getType()->getAs<RecordType>())
      if (const RecordDecl *child = childType->getDecl())
        if (!validateRecord(child))
          return false;
  } @_27_
  return true;
} @_26_

class IndexDataConsumer : public index::IndexDataConsumer { @_28_
public:
  ASTContext *ctx;
  IndexParam &param;

  std::string getComment(const Decl *d) { @_29_
    SourceManager &sm = ctx->getSourceManager();
    const RawComment *rc = ctx->getRawCommentForAnyRedecl(d);
    if (!rc)
      return "";
    StringRef raw = rc->getRawText(ctx->getSourceManager());
    SourceRange sr = rc->getSourceRange();
    std::pair<FileID, unsigned> bInfo = sm.getDecomposedLoc(sr.getBegin());
    unsigned start_column = sm.getLineNumber(bInfo.first, bInfo.second);
    std::string ret;
    int pad = -1;
    for (const char *p = raw.data(), *e = raw.end(); p < e;) { @_30_
      // The first line starts with a comment marker, but the rest needs
      // un-indenting.
      unsigned skip = start_column - 1;
      for (; skip > 0 && p < e && (*p == ' ' || *p == '\t'); p++)
        skip--;
      const char *q = p;
      while (q < e && *q != '\n')
        q++;
      if (q < e)
        q++;
      // A minimalist approach to skip Doxygen comment markers.
      // See https://www.stack.nl/~dimitri/doxygen/manual/docblocks.html
      if (pad < 0) { @_31_
        // First line, detect the length of comment marker and put into |pad|
        const char *begin = p;
        while (p < e && (*p == '/' || *p == '*' || *p == '-' || *p == '='))
          p++;
        if (p < e && (*p == '<' || *p == '!'))
          p++;
        if (p < e && *p == ' ')
          p++;
        if (p + 1 == q)
          p++;
        else
          pad = int(p - begin);
      } else {
        // Other lines, skip |pad| bytes
        int prefix = pad;
        while (prefix > 0 && p < e &&
               (*p == ' ' || *p == '/' || *p == '*' || *p == '<' || *p == '!'))
          prefix--, p++;
      } @_31_
      ret.insert(ret.end(), p, q);
      p = q;
    } @_30_
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    if (StringRef(ret).endswith("*/") || StringRef(ret).endswith("\n/"))
      ret.resize(ret.size() - 2);
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    return ret;
  } @_29_

  Usr getUsr(const Decl *d, IndexParam::DeclInfo **info = nullptr) const { @_32_
    d = d->getCanonicalDecl();
    auto [it, inserted] = param.decl2Info.try_emplace(d);
    if (inserted) { @_33_
      SmallString<256> usr;
      index::generateUSRForDecl(d, usr);
      auto &info = it->second;
      info.usr = hashUsr(usr);
      if (auto *nd = dyn_cast<NamedDecl>(d)) { @_34_
        info.short_name = nd->getNameAsString();
        llvm::raw_string_ostream os(info.qualified);
        nd->printQualifiedName(os, getDefaultPolicy());
        simplifyAnonymous(info.qualified);
      } @_34_
    } @_33_
    if (info)
      *info = &it->second;
    return it->second.usr;
  } @_32_

  PrintingPolicy getDefaultPolicy() const { @_35_
    PrintingPolicy pp(ctx->getLangOpts());
    pp.AnonymousTagLocations = false;
    pp.TerseOutput = true;
    pp.PolishForDeclaration = true;
    pp.ConstantsAsWritten = true;
    pp.SuppressTagKeyword = true;
    pp.SuppressUnwrittenScope = g_config->index.name.suppressUnwrittenScope;
    pp.SuppressInitializers = true;
    pp.FullyQualifiedName = false;
    return pp;
  } @_35_

  static void simplifyAnonymous(std::string &name) { @_36_
    for (std::string::size_type i = 0;;) { @_37_
      if ((i = name.find("(anonymous ", i)) == std::string::npos)
        break;
      i++;
      if (name.size() - i > 19 && name.compare(i + 10, 9, "namespace") == 0)
        name.replace(i, 19, "anon ns");
      else
        name.replace(i, 9, "anon");
    } @_37_
  } @_36_

  template <typename Def>
  void setName(const Decl *d, std::string_view short_name,
               std::string_view qualified, Def &def) { @_38_
    SmallString<256> str;
    llvm::raw_svector_ostream os(str);
    d->print(os, getDefaultPolicy());

    std::string name(str.data(), str.size());
    simplifyAnonymous(name);
    // Remove \n in DeclPrinter.cpp "{\n" + if(!TerseOutput)something + "}"
    for (std::string::size_type i = 0;;) { @_39_
      if ((i = name.find("{\n}", i)) == std::string::npos)
        break;
      name.replace(i, 3, "{}");
    } @_39_
    auto i = name.find(short_name);
    if (short_name.size())
      while (i != std::string::npos &&
             ((i && isIdentifierBody(name[i - 1])) ||
              isIdentifierBody(name[i + short_name.size()])))
        i = name.find(short_name, i + short_name.size());
    if (i == std::string::npos) { @_40_
      // e.g. operator type-parameter-1
      i = 0;
      def.short_name_offset = 0;
    } else if (short_name.empty() || (i >= 2 && name[i - 2] == ':')) {
      // Don't replace name with qualified name in ns::name Cls::*name
      def.short_name_offset = i;
    } else {
      name.replace(i, short_name.size(), qualified);
      def.short_name_offset = i + qualified.size() - short_name.size();
    } @_40_
    // name may be empty while short_name is not.
    def.short_name_size = name.empty() ? 0 : short_name.size();
    for (int paren = 0; i; i--) { @_41_
      // Skip parentheses in "(anon struct)::name"
      if (name[i - 1] == ')')
        paren++;
      else if (name[i - 1] == '(')
        paren--;
      else if (!(paren > 0 || isIdentifierBody(name[i - 1]) ||
                 name[i - 1] == ':'))
        break;
    } @_41_
    def.qual_name_offset = i;
    def.detailed_name = intern(name);
  } @_38_

  void setVarName(const Decl *d, std::string_view short_name,
                  std::string_view qualified, IndexVar::Def &def) { @_42_
    QualType t;
    const Expr *init = nullptr;
    bool deduced = false;
    if (auto *vd = dyn_cast<VarDecl>(d)) { @_43_
      t = vd->getType();
      init = vd->getAnyInitializer();
      def.storage = vd->getStorageClass();
    } else if (auto *fd = dyn_cast<FieldDecl>(d)) {
      t = fd->getType();
      init = fd->getInClassInitializer();
    } else if (auto *bd = dyn_cast<BindingDecl>(d)) {
      t = bd->getType();
      deduced = true;
    } @_43_
    if (!t.isNull()) { @_44_
      if (t->getContainedDeducedType()) { @_45_
        deduced = true;
      } else if (auto *dt = dyn_cast<DecltypeType>(t)) {
        // decltype(y) x;
        while (dt && !dt->getUnderlyingType().isNull()) { @_46_
          t = dt->getUnderlyingType();
          dt = dyn_cast<DecltypeType>(t);
        } @_46_
        deduced = true;
      } @_45_
    } @_44_
    if (!t.isNull() && deduced) { @_47_
      SmallString<256> str;
      llvm::raw_svector_ostream os(str);
      PrintingPolicy pp = getDefaultPolicy();
      t.print(os, pp);
      if (str.size() &&
          (str.back() != ' ' && str.back() != '*' && str.back() != '&'))
        str += ' ';
      def.qual_name_offset = str.size();
      def.short_name_offset = str.size() + qualified.size() - short_name.size();
      def.short_name_size = short_name.size();
      str += StringRef(qualified.data(), qualified.size());
      def.detailed_name = intern(str);
    } else {
      setName(d, short_name, qualified, def);
    } @_47_
    if (init) { @_48_
      SourceManager &sm = ctx->getSourceManager();
      const LangOptions &lang = ctx->getLangOpts();
      SourceRange sr =
          sm.getExpansionRange(init->getSourceRange()).getAsRange();
      SourceLocation l = d->getLocation();
      if (l.isMacroID() || !sm.isBeforeInTranslationUnit(l, sr.getBegin()))
        return;
      StringRef buf = getSourceInRange(sm, lang, sr);
      Twine init = buf.count('\n') <= g_config->index.maxInitializerLines - 1
                       ? buf.size() && buf[0] == ':' ? Twine(" ", buf)
                                                     : Twine(" = ", buf)
                       : Twine();
      Twine t = def.detailed_name + init;
      def.hover =
          def.storage == SC_Static && strncmp(def.detailed_name, "static ", 7)
              ? intern(("static " + t).str())
              : intern(t.str());
    } @_48_
  } @_42_

  static int getFileLID(IndexFile *db, SourceManager &sm, FileID fid) { @_49_
    auto [it, inserted] = db->uid2lid_and_path.try_emplace(fid);
    if (inserted) { @_50_
      const FileEntry *fe = sm.getFileEntryForID(fid);
      if (!fe) { @_51_
        it->second.first = -1;
        return -1;
      } @_51_
      it->second.first = db->uid2lid_and_path.size() - 1;
      it->second.second = pathFromFileEntry(*fe);
    } @_50_
    return it->second.first;
  } @_49_

  void addMacroUse(IndexFile *db, SourceManager &sm, Usr usr, Kind kind,
                   SourceLocation sl) const { @_52_
    FileID fid = sm.getFileID(sl);
    int lid = getFileLID(db, sm, fid);
    if (lid < 0)
      return;
    Range spell = fromTokenRange(sm, ctx->getLangOpts(), SourceRange(sl, sl));
    Use use{{spell, Role::Dynamic}, lid};
    switch (kind) { @_53_
    case Kind::Func:
      db->toFunc(usr).uses.push_back(use);
      break;
    case Kind::Type:
      db->toType(usr).uses.push_back(use);
      break;
    case Kind::Var:
      db->toVar(usr).uses.push_back(use);
      break;
    default:
      llvm_unreachable("");
    } @_53_
  } @_52_

  void collectRecordMembers(IndexType &type, const RecordDecl *rd) { @_54_
    SmallVector<std::pair<const RecordDecl *, int>, 2> stack{{rd, 0}};
    llvm::DenseSet<const RecordDecl *> seen;
    seen.insert(rd);
    while (stack.size()) { @_55_
      int offset;
      std::tie(rd, offset) = stack.back();
      stack.pop_back();
      if (!rd->isCompleteDefinition() || rd->isDependentType() ||
          rd->isInvalidDecl() || !validateRecord(rd))
        offset = -1;
      for (FieldDecl *fd : rd->fields()) { @_56_
        int offset1 = offset < 0 ? -1 : int(offset + ctx->getFieldOffset(fd));
        if (fd->getIdentifier())
          type.def.vars.emplace_back(getUsr(fd), offset1);
        else if (const auto *rt1 = fd->getType()->getAs<RecordType>()) { @_57_
          if (const RecordDecl *rd1 = rt1->getDecl())
            if (seen.insert(rd1).second)
              stack.push_back({rd1, offset1});
        } @_57_
      } @_56_
    } @_55_
  } @_54_

public:
  IndexDataConsumer(IndexParam &param) : param(param) {}
  void initialize(ASTContext &ctx) override { this->ctx = param.ctx = &ctx; }
#if LLVM_VERSION_MAJOR < 10 // llvmorg-10-init-12036-g3b9715cb219 @_58_
# define handleDeclOccurrence handleDeclOccurence
#endif @_58_
  bool handleDeclOccurrence(const Decl *d, index::SymbolRoleSet roles,
                            ArrayRef<index::SymbolRelation> relations,
                            SourceLocation src_loc,
                            ASTNodeInfo ast_node) override { @_59_
    if (!param.no_linkage) { @_60_
      if (auto *nd = dyn_cast<NamedDecl>(d); nd && nd->hasLinkage())
        ;
      else
        return true;
    } @_60_
    SourceManager &sm = ctx->getSourceManager();
    const LangOptions &lang = ctx->getLangOpts();
    FileID fid;
    SourceLocation spell = sm.getSpellingLoc(src_loc);
    Range loc;
    auto r = sm.isMacroArgExpansion(src_loc)
                 ? CharSourceRange::getTokenRange(spell)
                 : sm.getExpansionRange(src_loc);
    loc = fromCharSourceRange(sm, lang, r);
    fid = sm.getFileID(r.getBegin());
    if (fid.isInvalid())
      return true;
    int lid = -1;
    IndexFile *db;
    if (g_config->index.multiVersion && param.useMultiVersion(fid)) { @_61_
      db = param.consumeFile(sm.getMainFileID());
      if (!db)
        return true;
      param.seenFile(fid);
      if (!sm.isWrittenInMainFile(r.getBegin()))
        lid = getFileLID(db, sm, fid);
    } else {
      db = param.consumeFile(fid);
      if (!db)
        return true;
    } @_61_

    // spell, extent, comments use OrigD while most others use adjusted |D|.
    const Decl *origD = ast_node.OrigD;
    const DeclContext *sem_dc = origD->getDeclContext()->getRedeclContext();
    const DeclContext *lex_dc = ast_node.ContainerDC->getRedeclContext();
    { @_62_
      const NamespaceDecl *nd;
      while ((nd = dyn_cast<NamespaceDecl>(cast<Decl>(sem_dc))) &&
             nd->isAnonymousNamespace())
        sem_dc = nd->getDeclContext()->getRedeclContext();
      while ((nd = dyn_cast<NamespaceDecl>(cast<Decl>(lex_dc))) &&
             nd->isAnonymousNamespace())
        lex_dc = nd->getDeclContext()->getRedeclContext();
    } @_62_
    Role role = static_cast<Role>(roles);
    db->language = LanguageId((int)db->language | (int)getDeclLanguage(d));

    bool is_decl = roles & uint32_t(index::SymbolRole::Declaration);
    bool is_def = roles & uint32_t(index::SymbolRole::Definition);
    if (is_decl && d->getKind() == Decl::Binding)
      is_def = true;
    IndexFunc *func = nullptr;
    IndexType *type = nullptr;
    IndexVar *var = nullptr;
    SymbolKind ls_kind = SymbolKind::Unknown;
    Kind kind = getKind(d, ls_kind);

    if (is_def)
      switch (d->getKind()) { @_63_
      case Decl::CXXConversion: // *operator* int => *operator int*
      case Decl::CXXDestructor: // *~*A => *~A*
      case Decl::CXXMethod:     // *operator*= => *operator=*
      case Decl::Function:      // operator delete
        if (src_loc.isFileID()) { @_64_
          SourceRange sr =
              cast<FunctionDecl>(origD)->getNameInfo().getSourceRange();
          if (sr.getEnd().isFileID())
            loc = fromTokenRange(sm, lang, sr);
        } @_64_
        break;
      default:
        break;
      } @_63_
    else { @_65_
      // e.g. typedef Foo<int> gg; => Foo has an unadjusted `D`
      const Decl *d1 = getAdjustedDecl(d);
      if (d1 && d1 != d)
        d = d1;
    } @_65_

    IndexParam::DeclInfo *info;
    Usr usr = getUsr(d, &info);

    auto do_def_decl = [&](auto *entity) { @_66_
      Use use{{loc, role}, lid};
      if (is_def) { @_67_
        SourceRange sr = origD->getSourceRange();
        entity->def.spell = {use, @_68_
                             fromTokenRangeDefaulted(sm, lang, sr, fid, loc)}; @_68_
        entity->def.parent_kind = SymbolKind::File;
        getKind(cast<Decl>(sem_dc), entity->def.parent_kind);
      } else if (is_decl) {
        SourceRange sr = origD->getSourceRange();
        entity->declarations.push_back(
            {use, fromTokenRangeDefaulted(sm, lang, sr, fid, loc)});
      } else {
        entity->uses.push_back(use);
        return;
      } @_67_
      if (entity->def.comments[0] == '\0' && g_config->index.comments)
        entity->def.comments = intern(getComment(origD));
    }; @_66_
    switch (kind) { @_69_
    case Kind::Invalid:
      if (ls_kind == SymbolKind::Unknown)
        LOG_S(INFO) << "Unhandled " << int(d->getKind()) << " "
                    << info->qualified << " in " << db->path << ":"
                    << (loc.start.line + 1) << ":" << (loc.start.column + 1);
      return true;
    case Kind::File:
      return true;
    case Kind::Func:
      func = &db->toFunc(usr);
      func->def.kind = ls_kind;
      // Mark as Role::Implicit to span one more column to the left/right.
      if (!is_def && !is_decl &&
          (d->getKind() == Decl::CXXConstructor ||
           d->getKind() == Decl::CXXConversion))
        role = Role(role | Role::Implicit);
      do_def_decl(func);
      if (spell != src_loc)
        addMacroUse(db, sm, usr, Kind::Func, spell);
      if (func->def.detailed_name[0] == '\0')
        setName(d, info->short_name, info->qualified, func->def);
      if (is_def || is_decl) { @_70_
        const Decl *dc = cast<Decl>(sem_dc);
        if (getKind(dc, ls_kind) == Kind::Type)
          db->toType(getUsr(dc)).def.funcs.push_back(usr);
      } else {
        const Decl *dc = cast<Decl>(lex_dc);
        if (getKind(dc, ls_kind) == Kind::Func)
          db->toFunc(getUsr(dc))
              .def.callees.push_back({loc, usr, Kind::Func, role});
      } @_70_
      break;
    case Kind::Type:
      type = &db->toType(usr);
      type->def.kind = ls_kind;
      do_def_decl(type);
      if (spell != src_loc)
        addMacroUse(db, sm, usr, Kind::Type, spell);
      if ((is_def || type->def.detailed_name[0] == '\0') &&
          info->short_name.size()) { @_71_
        if (d->getKind() == Decl::TemplateTypeParm)
          type->def.detailed_name = intern(info->short_name);
        else
          // OrigD may be detailed, e.g. "struct D : B {}"
          setName(origD, info->short_name, info->qualified, type->def);
      } @_71_
      if (is_def || is_decl) { @_72_
        const Decl *dc = cast<Decl>(sem_dc);
        if (getKind(dc, ls_kind) == Kind::Type)
          db->toType(getUsr(dc)).def.types.push_back(usr);
      } @_72_
      break;
    case Kind::Var:
      var = &db->toVar(usr);
      var->def.kind = ls_kind;
      do_def_decl(var);
      if (spell != src_loc)
        addMacroUse(db, sm, usr, Kind::Var, spell);
      if (var->def.detailed_name[0] == '\0')
        setVarName(d, info->short_name, info->qualified, var->def);
      QualType t;
      if (auto *vd = dyn_cast<ValueDecl>(d))
        t = vd->getType();
      if (is_def || is_decl) { @_73_
        const Decl *dc = cast<Decl>(sem_dc);
        Kind kind = getKind(dc, var->def.parent_kind);
        if (kind == Kind::Func)
          db->toFunc(getUsr(dc)).def.vars.push_back(usr);
        else if (kind == Kind::Type && !isa<RecordDecl>(sem_dc))
          db->toType(getUsr(dc)).def.vars.emplace_back(usr, -1);
        if (!t.isNull()) { @_74_
          if (auto *bt = t->getAs<BuiltinType>()) { @_75_
            Usr usr1 = static_cast<Usr>(bt->getKind());
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(d))
              db->toType(usr1).instances.push_back(usr);
          } else if (const Decl *d1 = getAdjustedDecl(getTypeDecl(t))) {
#if LLVM_VERSION_MAJOR < 9 @_76_
            if (isa<TemplateTypeParmDecl>(d1)) { @_77_
              // e.g. TemplateTypeParmDecl is not handled by
              // handleDeclOccurence.
              SourceRange sr1 = d1->getSourceRange();
              if (sm.getFileID(sr1.getBegin()) == fid) { @_78_
                IndexParam::DeclInfo *info1;
                Usr usr1 = getUsr(d1, &info1);
                IndexType &type1 = db->toType(usr1);
                SourceLocation sl1 = d1->getLocation();
                type1.def.spell = { @_79_
                    Use{{fromTokenRange(sm, lang, {sl1, sl1}), Role::Definition}, @_80_
                        lid}, @_80_
                    fromTokenRange(sm, lang, sr1)}; @_79_
                type1.def.detailed_name = intern(info1->short_name);
                type1.def.short_name_size = int16_t(info1->short_name.size());
                type1.def.kind = SymbolKind::TypeParameter;
                type1.def.parent_kind = SymbolKind::Class;
                var->def.type = usr1;
                type1.instances.push_back(usr);
                break;
              } @_78_
            } @_77_
#endif @_76_

            IndexParam::DeclInfo *info1;
            Usr usr1 = getUsr(d1, &info1);
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(d))
              db->toType(usr1).instances.push_back(usr);
          } @_75_
        } @_74_
      } else if (!var->def.spell && var->declarations.empty()) {
        // e.g. lambda parameter
        SourceLocation l = d->getLocation();
        if (sm.getFileID(l) == fid) { @_81_
          var->def.spell = { @_82_
              Use{{fromTokenRange(sm, lang, {l, l}), Role::Definition}, lid},
              fromTokenRange(sm, lang, d->getSourceRange())}; @_82_
          var->def.parent_kind = SymbolKind::Method;
        } @_81_
      } @_73_
      break;
    } @_69_

    switch (d->getKind()) { @_83_
    case Decl::Namespace:
      if (d->isFirstDecl()) { @_84_
        auto *nd = cast<NamespaceDecl>(d);
        auto *nd1 = cast<Decl>(nd->getParent());
        if (isa<NamespaceDecl>(nd1)) { @_85_
          Usr usr1 = getUsr(nd1);
          type->def.bases.push_back(usr1);
          db->toType(usr1).derived.push_back(usr);
        } @_85_
      } @_84_
      break;
    case Decl::NamespaceAlias: { @_86_
      auto *nad = cast<NamespaceAliasDecl>(d);
      if (const NamespaceDecl *nd = nad->getNamespace()) { @_87_
        Usr usr1 = getUsr(nd);
        type->def.alias_of = usr1;
        (void)db->toType(usr1);
      } @_87_
      break;
    } @_86_
    case Decl::CXXRecord:
      if (is_def) { @_88_
        auto *rd = dyn_cast<CXXRecordDecl>(d);
        if (rd && rd->hasDefinition())
          for (const CXXBaseSpecifier &base : rd->bases())
            if (const Decl *baseD =
                    getAdjustedDecl(getTypeDecl(base.getType()))) { @_89_
              Usr usr1 = getUsr(baseD);
              type->def.bases.push_back(usr1);
              db->toType(usr1).derived.push_back(usr);
            } @_89_
      } @_88_
      [[fallthrough]];
    case Decl::Enum:
    case Decl::Record:
      if (auto *tag_d = dyn_cast<TagDecl>(d)) { @_90_
        if (type->def.detailed_name[0] == '\0' && info->short_name.empty()) { @_91_
          StringRef tag;
          switch (tag_d->getTagKind()) { @_92_
          case TTK_Struct:
            tag = "struct";
            break;
          case TTK_Interface:
            tag = "__interface";
            break;
          case TTK_Union:
            tag = "union";
            break;
          case TTK_Class:
            tag = "class";
            break;
          case TTK_Enum:
            tag = "enum";
            break;
          } @_92_
          if (TypedefNameDecl *td = tag_d->getTypedefNameForAnonDecl()) { @_93_
            StringRef name = td->getName();
            std::string detailed = ("anon " + tag + " " + name).str();
            type->def.detailed_name = intern(detailed);
            type->def.short_name_size = detailed.size();
          } else {
            std::string name = ("anon " + tag).str();
            type->def.detailed_name = intern(name);
            type->def.short_name_size = name.size();
          } @_93_
        } @_91_
        if (is_def && !isa<EnumDecl>(d))
          if (auto *ord = dyn_cast<RecordDecl>(origD))
            collectRecordMembers(*type, ord);
      } @_90_
      break;
    case Decl::ClassTemplateSpecialization:
    case Decl::ClassTemplatePartialSpecialization:
      type->def.kind = SymbolKind::Class;
      if (is_def) { @_94_
        if (auto *ord = dyn_cast<RecordDecl>(origD))
          collectRecordMembers(*type, ord);
        if (auto *rd = dyn_cast<CXXRecordDecl>(d)) { @_95_
          Decl *d1 = nullptr;
          if (auto *sd = dyn_cast<ClassTemplatePartialSpecializationDecl>(rd))
            d1 = sd->getSpecializedTemplate();
          else if (auto *sd = dyn_cast<ClassTemplateSpecializationDecl>(rd)) { @_96_
            llvm::PointerUnion<ClassTemplateDecl *,
                               ClassTemplatePartialSpecializationDecl *>
                result = sd->getSpecializedTemplateOrPartial();
            if (result.is<ClassTemplateDecl *>())
              d1 = result.get<ClassTemplateDecl *>();
            else
              d1 = result.get<ClassTemplatePartialSpecializationDecl *>();

          } else @_96_
            d1 = rd->getInstantiatedFromMemberClass();
          if (d1) { @_97_
            Usr usr1 = getUsr(d1);
            type->def.bases.push_back(usr1);
            db->toType(usr1).derived.push_back(usr);
          } @_97_
        } @_95_
      } @_94_
      break;
    case Decl::TypeAlias:
    case Decl::Typedef:
    case Decl::UnresolvedUsingTypename:
      if (auto *td = dyn_cast<TypedefNameDecl>(d)) { @_98_
        bool specialization = false;
        QualType t = td->getUnderlyingType();
        if (const Decl *d1 = getAdjustedDecl(getTypeDecl(t, &specialization))) { @_99_
          Usr usr1 = getUsr(d1);
          IndexType &type1 = db->toType(usr1);
          type->def.alias_of = usr1;
          // Not visited template<class T> struct B {typedef A<T> t;};
          if (specialization) { @_100_
            const TypeSourceInfo *tsi = td->getTypeSourceInfo();
            SourceLocation l1 = tsi->getTypeLoc().getBeginLoc();
            if (sm.getFileID(l1) == fid)
              type1.uses.push_back(
                  {{fromTokenRange(sm, lang, {l1, l1}), Role::Reference}, lid});
          } @_100_
        } @_99_
      } @_98_
      break;
    case Decl::CXXMethod:
      if (is_def || is_decl) { @_101_
        if (auto *nd = dyn_cast<NamedDecl>(d)) { @_102_
          SmallVector<const NamedDecl *, 8> overDecls;
          ctx->getOverriddenMethods(nd, overDecls);
          for (const auto *nd1 : overDecls) { @_103_
            Usr usr1 = getUsr(nd1);
            func->def.bases.push_back(usr1);
            db->toFunc(usr1).derived.push_back(usr);
          } @_103_
        } @_102_
      } @_101_
      break;
    case Decl::EnumConstant:
      if (is_def && strchr(var->def.detailed_name, '=') == nullptr) { @_104_
        auto *ecd = cast<EnumConstantDecl>(d);
        const auto &val = ecd->getInitVal();
        std::string init =
            " = " + (val.isSigned() ? std::to_string(val.getSExtValue())
                                    : std::to_string(val.getZExtValue()));
        var->def.hover = intern(var->def.detailed_name + init);
      } @_104_
      break;
    default:
      break;
    } @_83_
    return true;
  } @_59_
}; @_28_

class IndexPPCallbacks : public PPCallbacks { @_105_
  SourceManager &sm;
  IndexParam &param;

  std::pair<StringRef, Usr> getMacro(const Token &tok) const { @_106_
    StringRef name = tok.getIdentifierInfo()->getName();
    SmallString<256> usr("@macro@");
    usr += name;
    return {name, hashUsr(usr)};
  } @_106_

public:
  IndexPPCallbacks(SourceManager &sm, IndexParam &param)
      : sm(sm), param(param) {}
  void FileChanged(SourceLocation sl, FileChangeReason reason,
                   SrcMgr::CharacteristicKind, FileID) override { @_107_
    if (reason == FileChangeReason::EnterFile)
      (void)param.consumeFile(sm.getFileID(sl));
  } @_107_
  void InclusionDirective(SourceLocation hashLoc, const Token &tok,
                          StringRef included, bool isAngled,
                          CharSourceRange filenameRange, const FileEntry *file,
                          StringRef searchPath, StringRef relativePath,
                          const Module *imported,
                          SrcMgr::CharacteristicKind fileType) override { @_108_
    if (!file)
      return;
    auto spell = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                     filenameRange, nullptr);
    FileID fid = sm.getFileID(filenameRange.getBegin());
    if (IndexFile *db = param.consumeFile(fid)) { @_109_
      std::string path = pathFromFileEntry(*file);
      if (path.size())
        db->includes.push_back({spell.start.line, intern(path)});
    } @_109_
  } @_108_
  void MacroDefined(const Token &tok, const MacroDirective *md) override { @_110_
    const LangOptions &lang = param.ctx->getLangOpts();
    SourceLocation sl = md->getLocation();
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) { @_111_
      auto [name, usr] = getMacro(tok);
      IndexVar &var = db->toVar(usr);
      Range range = fromTokenRange(sm, lang, {sl, sl}, nullptr);
      var.def.kind = SymbolKind::Macro;
      var.def.parent_kind = SymbolKind::File;
      if (var.def.spell)
        var.declarations.push_back(*var.def.spell);
      const MacroInfo *mi = md->getMacroInfo();
      SourceRange sr(mi->getDefinitionLoc(), mi->getDefinitionEndLoc());
      Range extent = fromTokenRange(sm, param.ctx->getLangOpts(), sr);
      var.def.spell = {Use{{range, Role::Definition}}, extent};
      if (var.def.detailed_name[0] == '\0') { @_112_
        var.def.detailed_name = intern(name);
        var.def.short_name_size = name.size();
        StringRef buf = getSourceInRange(sm, lang, sr);
        var.def.hover =
            intern(buf.count('\n') <= g_config->index.maxInitializerLines - 1
                       ? Twine("#define ", getSourceInRange(sm, lang, sr)).str()
                       : Twine("#define ", name).str());
      } @_112_
    } @_111_
  } @_110_
  void MacroExpands(const Token &tok, const MacroDefinition &, SourceRange sr,
                    const MacroArgs *) override { @_113_
    SourceLocation sl = sm.getSpellingLoc(sr.getBegin());
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) { @_114_
      IndexVar &var = db->toVar(getMacro(tok).second);
      var.uses.push_back(
          {{fromTokenRange(sm, param.ctx->getLangOpts(), {sl, sl}, nullptr), @_115_ @_116_
            Role::Dynamic}}); @_115_ @_116_
    } @_114_
  } @_113_
  void MacroUndefined(const Token &tok, const MacroDefinition &md,
                      const MacroDirective *ud) override { @_117_
    if (ud) { @_118_
      SourceLocation sl = ud->getLocation();
      MacroExpands(tok, md, {sl, sl}, nullptr);
    } @_118_
  } @_117_
  void SourceRangeSkipped(SourceRange sr, SourceLocation) override { @_119_
    Range range = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                      CharSourceRange::getCharRange(sr));
    FileID fid = sm.getFileID(sr.getBegin());
    if (fid.isValid())
      if (IndexFile *db = param.consumeFile(fid))
        db->skipped_ranges.push_back(range);
  } @_119_
}; @_105_

class IndexFrontendAction : public ASTFrontendAction { @_120_
  std::shared_ptr<IndexDataConsumer> dataConsumer;
  const index::IndexingOptions &indexOpts;
  IndexParam &param;

public:
  IndexFrontendAction(std::shared_ptr<IndexDataConsumer> dataConsumer,
                      const index::IndexingOptions &indexOpts,
                      IndexParam &param)
      : dataConsumer(std::move(dataConsumer)), indexOpts(indexOpts),
        param(param) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &ci,
                                                 StringRef inFile) override { @_121_
    class SkipProcessed : public ASTConsumer { @_122_
      IndexParam &param;
      const ASTContext *ctx = nullptr;

    public:
      SkipProcessed(IndexParam &param) : param(param) {}
      void Initialize(ASTContext &ctx) override { this->ctx = &ctx; }
      bool shouldSkipFunctionBody(Decl *d) override { @_123_
        const SourceManager &sm = ctx->getSourceManager();
        FileID fid = sm.getFileID(sm.getExpansionLoc(d->getLocation()));
        return !(g_config->index.multiVersion && param.useMultiVersion(fid)) &&
               !param.consumeFile(fid);
      } @_123_
    }; @_122_

    std::shared_ptr<Preprocessor> pp = ci.getPreprocessorPtr();
    pp->addPPCallbacks(
        std::make_unique<IndexPPCallbacks>(pp->getSourceManager(), param));
    std::vector<std::unique_ptr<ASTConsumer>> consumers;
    consumers.push_back(std::make_unique<SkipProcessed>(param));
#if LLVM_VERSION_MAJOR >= 10 // rC370337 @_124_
    consumers.push_back(index::createIndexingASTConsumer(
        dataConsumer, indexOpts, std::move(pp)));
#endif @_124_
    return std::make_unique<MultiplexConsumer>(std::move(consumers));
  } @_121_
}; @_120_
} // namespace @_1_

const int IndexFile::kMajorVersion = 21;
const int IndexFile::kMinorVersion = 0;

IndexFile::IndexFile(const std::string &path, const std::string &contents,
                     bool no_linkage)
    : path(path), no_linkage(no_linkage), file_contents(contents) {}

IndexFunc &IndexFile::toFunc(Usr usr) { @_125_
  auto [it, inserted] = usr2func.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_125_

IndexType &IndexFile::toType(Usr usr) { @_126_
  auto [it, inserted] = usr2type.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_126_

IndexVar &IndexFile::toVar(Usr usr) { @_127_
  auto [it, inserted] = usr2var.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_127_

std::string IndexFile::toString() { @_128_
  return ccls::serialize(SerializeFormat::Json, *this);
} @_128_

template <typename T> void uniquify(std::vector<T> &a) { @_129_
  std::unordered_set<T> seen;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); i++)
    if (seen.insert(a[i]).second)
      a[n++] = a[i];
  a.resize(n);
} @_129_

namespace idx { @_130_
void init() { @_131_
  multiVersionMatcher = new GroupMatch(g_config->index.multiVersionWhitelist,
                                       g_config->index.multiVersionBlacklist);
} @_131_

std::vector<std::unique_ptr<IndexFile>>
index(SemaManager *manager, WorkingFiles *wfiles, VFS *vfs,
      const std::string &opt_wdir, const std::string &main,
      const std::vector<const char *> &args,
      const std::vector<std::pair<std::string, std::string>> &remapped,
      bool no_linkage, bool &ok) { @_132_
  ok = true;
  auto pch = std::make_shared<PCHContainerOperations>();
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs =
      llvm::vfs::getRealFileSystem();
  std::shared_ptr<CompilerInvocation> ci =
      buildCompilerInvocation(main, args, fs);
  // e.g. .s
  if (!ci)
    return {};
  ok = false;
  // -fparse-all-comments enables documentation in the indexer and in
  // code completion.
  ci->getLangOpts()->CommentOpts.ParseAllComments =
      g_config->index.comments > 1;
  ci->getLangOpts()->RetainCommentsFromSystemHeaders = true;
  std::string buf = wfiles->getContent(main);
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> bufs;
  if (buf.size())
    for (auto &[filename, content] : remapped) { @_133_
      bufs.push_back(llvm::MemoryBuffer::getMemBuffer(content));
      ci->getPreprocessorOpts().addRemappedFile(filename, bufs.back().get());
    } @_133_

  DiagnosticConsumer dc;
  auto clang = std::make_unique<CompilerInstance>(pch);
  clang->setInvocation(std::move(ci));
  clang->createDiagnostics(&dc, false);
  clang->getDiagnostics().setIgnoreAllWarnings(true);
  clang->setTarget(TargetInfo::CreateTargetInfo(
      clang->getDiagnostics(), clang->getInvocation().TargetOpts));
  if (!clang->hasTarget())
    return {};
  clang->getPreprocessorOpts().RetainRemappedFileBuffers = true;
#if LLVM_VERSION_MAJOR >= 9 // rC357037 @_134_
  clang->createFileManager(fs); @_134_
#else @_135_
  clang->setVirtualFileSystem(fs);
  clang->createFileManager();
#endif @_135_
  clang->setSourceManager(new SourceManager(clang->getDiagnostics(),
                                            clang->getFileManager(), true));

  IndexParam param(*vfs, no_linkage);

  index::IndexingOptions indexOpts;
  indexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  if (no_linkage) { @_136_
    indexOpts.IndexFunctionLocals = true;
    indexOpts.IndexImplicitInstantiation = true;
#if LLVM_VERSION_MAJOR >= 9 @_137_

    indexOpts.IndexParametersInDeclarations =
        g_config->index.parametersInDeclarations;
    indexOpts.IndexTemplateParameters = true;
#endif @_137_
  } @_136_

#if LLVM_VERSION_MAJOR >= 10 // rC370337 @_138_
  auto action = std::make_unique<IndexFrontendAction>(
      std::make_shared<IndexDataConsumer>(param), indexOpts, param); @_138_
#else @_139_
  auto dataConsumer = std::make_shared<IndexDataConsumer>(param);
  auto action = createIndexingAction(
      dataConsumer, indexOpts,
      std::make_unique<IndexFrontendAction>(dataConsumer, indexOpts, param));
#endif @_139_

  std::string reason;
  { @_140_
    llvm::CrashRecoveryContext crc;
    auto parse = [&]() { @_141_
      if (!action->BeginSourceFile(*clang, clang->getFrontendOpts().Inputs[0]))
        return;
#if LLVM_VERSION_MAJOR >= 9 // rL364464 @_142_
      if (llvm::Error e = action->Execute()) { @_143_
        reason = llvm::toString(std::move(e));
        return;
      } @_142_ @_143_
#else @_144_
      if (!action->Execute())
        return;
#endif @_144_
      action->EndSourceFile();
      ok = true;
    }; @_141_
    if (!crc.RunSafely(parse)) { @_145_
      LOG_S(ERROR) << "clang crashed for " << main;
      return {};
    } @_145_
  } @_140_
  if (!ok) { @_146_
    LOG_S(ERROR) << "failed to index " << main
                 << (reason.empty() ? "" : ": " + reason);
    return {};
  } @_146_

  std::vector<std::unique_ptr<IndexFile>> result;
  for (auto &it : param.uid2file) { @_147_
    if (!it.second.db)
      continue;
    std::unique_ptr<IndexFile> &entry = it.second.db;
    entry->import_file = main;
    entry->args = args;
    for (auto &[_, it] : entry->uid2lid_and_path)
      if (it.first >= 0)
        entry->lid2path.emplace_back(it.first, std::move(it.second));
    entry->uid2lid_and_path.clear();
    for (auto &it : entry->usr2func) { @_148_
      // e.g. declaration + out-of-line definition
      uniquify(it.second.derived);
      uniquify(it.second.uses);
    } @_148_
    for (auto &it : entry->usr2type) { @_149_
      uniquify(it.second.derived);
      uniquify(it.second.uses);
      // e.g. declaration + out-of-line definition
      uniquify(it.second.def.bases);
      uniquify(it.second.def.funcs);
    } @_149_
    for (auto &it : entry->usr2var)
      uniquify(it.second.uses);

    // Update dependencies for the file.
    for (auto &[_, file] : param.uid2file) { @_150_
      const std::string &path = file.path;
      if (path.empty())
        continue;
      if (path == entry->path)
        entry->mtime = file.mtime;
      else if (path != entry->import_file)
        entry->dependencies[llvm::CachedHashStringRef(intern(path))] =
            file.mtime;
    } @_150_
    result.push_back(std::move(entry));
  } @_147_

  return result;
} @_132_
} // namespace idx @_130_

void reflect(JsonReader &vis, SymbolRef &v) { @_151_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.usr = strtoull(s + 1, &s, 10);
  v.kind = static_cast<Kind>(strtol(s + 1, &s, 10));
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
} @_151_
void reflect(JsonReader &vis, Use &v) { @_152_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
} @_152_
void reflect(JsonReader &vis, DeclRef &v) { @_153_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|') + 1;
  v.extent = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
} @_153_

void reflect(JsonWriter &vis, SymbolRef &v) { @_154_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%" PRIu64 "|%d|%d", v.range.toString().c_str(),
           v.usr, int(v.kind), int(v.role));
  std::string s(buf);
  reflect(vis, s);
} @_154_
void reflect(JsonWriter &vis, Use &v) { @_155_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%d|%d", v.range.toString().c_str(), int(v.role),
           v.file_id);
  std::string s(buf);
  reflect(vis, s);
} @_155_
void reflect(JsonWriter &vis, DeclRef &v) { @_156_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%s|%d|%d", v.range.toString().c_str(),
           v.extent.toString().c_str(), int(v.role), v.file_id);
  std::string s(buf);
  reflect(vis, s);
} @_156_

void reflect(BinaryReader &vis, SymbolRef &v) { @_157_
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
} @_157_
void reflect(BinaryReader &vis, Use &v) { @_158_
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
} @_158_
void reflect(BinaryReader &vis, DeclRef &v) { @_159_
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
} @_159_

void reflect(BinaryWriter &vis, SymbolRef &v) { @_160_
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
} @_160_
void reflect(BinaryWriter &vis, Use &v) { @_161_
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
} @_161_
void reflect(BinaryWriter &vis, DeclRef &v) { @_162_
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
} @_162_
} // namespace ccls @_0_
