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
  case Decl::LinkageSpec: @_12_
    return Kind::Invalid; @_12_
  case Decl::Namespace:
  case Decl::NamespaceAlias: @_13_
    kind = SymbolKind::Namespace;
    return Kind::Type; @_13_
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCImplementation:
  case Decl::ObjCInterface:
  case Decl::ObjCProtocol: @_14_
    kind = SymbolKind::Interface;
    return Kind::Type; @_14_
  case Decl::ObjCMethod: @_15_
    kind = SymbolKind::Method;
    return Kind::Func; @_15_
  case Decl::ObjCProperty: @_16_
    kind = SymbolKind::Property;
    return Kind::Type; @_16_
  case Decl::ClassTemplate: @_17_
    kind = SymbolKind::Class;
    return Kind::Type; @_17_
  case Decl::FunctionTemplate: @_18_
    kind = SymbolKind::Function;
    return Kind::Func; @_18_
  case Decl::TypeAliasTemplate: @_19_
    kind = SymbolKind::TypeAlias;
    return Kind::Type; @_19_
  case Decl::VarTemplate: @_20_
    kind = SymbolKind::Variable;
    return Kind::Var; @_20_
  case Decl::TemplateTemplateParm: @_21_
    kind = SymbolKind::TypeParameter;
    return Kind::Type; @_21_
  case Decl::Enum: @_22_
    kind = SymbolKind::Enum;
    return Kind::Type; @_22_
  case Decl::CXXRecord:
  case Decl::Record: @_23_
    kind = SymbolKind::Class;
    // spec has no Union, use Class
    if (auto *rd = dyn_cast<RecordDecl>(d))
      if (rd->getTagKind() == TTK_Struct)
        kind = SymbolKind::Struct;
    return Kind::Type; @_23_
  case Decl::ClassTemplateSpecialization:
  case Decl::ClassTemplatePartialSpecialization: @_24_
    kind = SymbolKind::Class;
    return Kind::Type; @_24_
  case Decl::TemplateTypeParm: @_25_
    kind = SymbolKind::TypeParameter;
    return Kind::Type; @_25_
  case Decl::TypeAlias:
  case Decl::Typedef:
  case Decl::UnresolvedUsingTypename: @_26_
    kind = SymbolKind::TypeAlias;
    return Kind::Type; @_26_
  case Decl::Using: @_27_
    kind = SymbolKind::Null; // ignored
    return Kind::Invalid; @_27_
  case Decl::Binding: @_28_
    kind = SymbolKind::Variable;
    return Kind::Var; @_28_
  case Decl::Field:
  case Decl::ObjCIvar: @_29_
    kind = SymbolKind::Field;
    return Kind::Var; @_29_
  case Decl::Function: @_30_
    kind = SymbolKind::Function;
    return Kind::Func; @_30_
  case Decl::CXXMethod: { @_31_ @_32_
    const auto *md = cast<CXXMethodDecl>(d);
    kind = md->isStatic() ? SymbolKind::StaticMethod : SymbolKind::Method;
    return Kind::Func;
  } @_31_ @_32_
  case Decl::CXXConstructor: @_33_
    kind = SymbolKind::Constructor;
    return Kind::Func; @_33_
  case Decl::CXXConversion:
  case Decl::CXXDestructor: @_34_
    kind = SymbolKind::Method;
    return Kind::Func; @_34_
  case Decl::NonTypeTemplateParm: @_35_
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var; @_35_
  case Decl::Var:
  case Decl::Decomposition: @_36_
    kind = SymbolKind::Variable;
    return Kind::Var; @_36_
  case Decl::ImplicitParam:
  case Decl::ParmVar: @_37_
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var; @_37_
  case Decl::VarTemplateSpecialization:
  case Decl::VarTemplatePartialSpecialization: @_38_
    kind = SymbolKind::Variable;
    return Kind::Var; @_38_
  case Decl::EnumConstant: @_39_
    kind = SymbolKind::EnumMember;
    return Kind::Var; @_39_
  case Decl::UnresolvedUsingValue: @_40_
    kind = SymbolKind::Variable;
    return Kind::Var; @_40_
  case Decl::TranslationUnit: @_41_
    return Kind::Invalid;

  default:
    return Kind::Invalid; @_41_
  } @_11_
} @_10_

LanguageId getDeclLanguage(const Decl *d) { @_42_
  switch (d->getKind()) { @_43_
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
  case Decl::ObjCTypeParam: @_44_
    return LanguageId::ObjC; @_44_
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
  case Decl::UsingShadow: @_45_
    return LanguageId::Cpp; @_45_
  } @_43_
} @_42_

// clang/lib/AST/DeclPrinter.cpp
QualType getBaseType(QualType t, bool deduce_auto) { @_46_
  QualType baseType = t;
  while (!baseType.isNull() && !baseType->isSpecifierType()) { @_47_
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
    else if (deduce_auto) { @_48_
      if (const AutoType *aTy = baseType->getAs<AutoType>())
        baseType = aTy->getDeducedType();
      else
        break;
    } else @_48_
      break;
  } @_47_
  return baseType;
} @_46_

const Decl *getTypeDecl(QualType t, bool *specialization = nullptr) { @_49_
  Decl *d = nullptr;
  t = getBaseType(t.getUnqualifiedType(), true);
  const Type *tp = t.getTypePtrOrNull();
  if (!tp)
    return nullptr;

try_again:
  switch (tp->getTypeClass()) { @_50_
  case Type::Typedef: @_51_
    d = cast<TypedefType>(tp)->getDecl();
    break; @_51_
  case Type::ObjCObject: @_52_
    d = cast<ObjCObjectType>(tp)->getInterface();
    break; @_52_
  case Type::ObjCInterface: @_53_
    d = cast<ObjCInterfaceType>(tp)->getDecl();
    break; @_53_
  case Type::Record:
  case Type::Enum: @_54_
    d = cast<TagType>(tp)->getDecl();
    break; @_54_
  case Type::TemplateTypeParm: @_55_
    d = cast<TemplateTypeParmType>(tp)->getDecl();
    break; @_55_
  case Type::TemplateSpecialization: @_56_
    if (specialization)
      *specialization = true;
    if (const RecordType *record = tp->getAs<RecordType>())
      d = record->getDecl();
    else
      d = cast<TemplateSpecializationType>(tp)
              ->getTemplateName()
              .getAsTemplateDecl();
    break;
 @_56_
  case Type::Auto:
  case Type::DeducedTemplateSpecialization: @_57_
    tp = cast<DeducedType>(tp)->getDeducedType().getTypePtrOrNull();
    if (tp)
      goto try_again;
    break;
 @_57_
  case Type::InjectedClassName: @_58_
    d = cast<InjectedClassNameType>(tp)->getDecl();
    break;

    // FIXME: Template type parameters!
 @_58_
  case Type::Elaborated: @_59_
    tp = cast<ElaboratedType>(tp)->getNamedType().getTypePtrOrNull();
    goto try_again;

  default:
    break; @_59_
  } @_50_
  return d;
} @_49_

const Decl *getAdjustedDecl(const Decl *d) { @_60_
  while (d) { @_61_
    if (auto *r = dyn_cast<CXXRecordDecl>(d)) { @_62_
      if (auto *s = dyn_cast<ClassTemplateSpecializationDecl>(r)) { @_63_
        if (!s->isExplicitSpecialization()) { @_64_
          llvm::PointerUnion<ClassTemplateDecl *,
                             ClassTemplatePartialSpecializationDecl *>
              result = s->getSpecializedTemplateOrPartial();
          if (result.is<ClassTemplateDecl *>())
            d = result.get<ClassTemplateDecl *>();
          else
            d = result.get<ClassTemplatePartialSpecializationDecl *>();
          continue;
        } @_64_
      } else if (auto *d1 = r->getInstantiatedFromMemberClass()) {
        d = d1;
        continue;
      } @_63_
    } else if (auto *ed = dyn_cast<EnumDecl>(d)) {
      if (auto *d1 = ed->getInstantiatedFromMemberEnum()) { @_65_
        d = d1;
        continue;
      } @_65_
    } @_62_
    break;
  } @_61_
  return d;
} @_60_

bool validateRecord(const RecordDecl *rd) { @_66_
  for (const auto *i : rd->fields()) { @_67_
    QualType fqt = i->getType();
    if (fqt->isIncompleteType() || fqt->isDependentType())
      return false;
    if (const RecordType *childType = i->getType()->getAs<RecordType>())
      if (const RecordDecl *child = childType->getDecl())
        if (!validateRecord(child))
          return false;
  } @_67_
  return true;
} @_66_

class IndexDataConsumer : public index::IndexDataConsumer { @_68_
public:
  ASTContext *ctx;
  IndexParam &param;

  std::string getComment(const Decl *d) { @_69_
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
    for (const char *p = raw.data(), *e = raw.end(); p < e;) { @_70_
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
      if (pad < 0) { @_71_
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
      } @_71_
      ret.insert(ret.end(), p, q);
      p = q;
    } @_70_
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    if (StringRef(ret).endswith("*/") || StringRef(ret).endswith("\n/"))
      ret.resize(ret.size() - 2);
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    return ret;
  } @_69_

  Usr getUsr(const Decl *d, IndexParam::DeclInfo **info = nullptr) const { @_72_
    d = d->getCanonicalDecl();
    auto [it, inserted] = param.decl2Info.try_emplace(d);
    if (inserted) { @_73_
      SmallString<256> usr;
      index::generateUSRForDecl(d, usr);
      auto &info = it->second;
      info.usr = hashUsr(usr);
      if (auto *nd = dyn_cast<NamedDecl>(d)) { @_74_
        info.short_name = nd->getNameAsString();
        llvm::raw_string_ostream os(info.qualified);
        nd->printQualifiedName(os, getDefaultPolicy());
        simplifyAnonymous(info.qualified);
      } @_74_
    } @_73_
    if (info)
      *info = &it->second;
    return it->second.usr;
  } @_72_

  PrintingPolicy getDefaultPolicy() const { @_75_
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
  } @_75_

  static void simplifyAnonymous(std::string &name) { @_76_
    for (std::string::size_type i = 0;;) { @_77_
      if ((i = name.find("(anonymous ", i)) == std::string::npos)
        break;
      i++;
      if (name.size() - i > 19 && name.compare(i + 10, 9, "namespace") == 0)
        name.replace(i, 19, "anon ns");
      else
        name.replace(i, 9, "anon");
    } @_77_
  } @_76_

  template <typename Def>
  void setName(const Decl *d, std::string_view short_name,
               std::string_view qualified, Def &def) { @_78_
    SmallString<256> str;
    llvm::raw_svector_ostream os(str);
    d->print(os, getDefaultPolicy());

    std::string name(str.data(), str.size());
    simplifyAnonymous(name);
    // Remove \n in DeclPrinter.cpp "{\n" + if(!TerseOutput)something + "}"
    for (std::string::size_type i = 0;;) { @_79_
      if ((i = name.find("{\n}", i)) == std::string::npos)
        break;
      name.replace(i, 3, "{}");
    } @_79_
    auto i = name.find(short_name);
    if (short_name.size())
      while (i != std::string::npos &&
             ((i && isIdentifierBody(name[i - 1])) ||
              isIdentifierBody(name[i + short_name.size()])))
        i = name.find(short_name, i + short_name.size());
    if (i == std::string::npos) { @_80_
      // e.g. operator type-parameter-1
      i = 0;
      def.short_name_offset = 0;
    } else if (short_name.empty() || (i >= 2 && name[i - 2] == ':')) {
      // Don't replace name with qualified name in ns::name Cls::*name
      def.short_name_offset = i;
    } else {
      name.replace(i, short_name.size(), qualified);
      def.short_name_offset = i + qualified.size() - short_name.size();
    } @_80_
    // name may be empty while short_name is not.
    def.short_name_size = name.empty() ? 0 : short_name.size();
    for (int paren = 0; i; i--) { @_81_
      // Skip parentheses in "(anon struct)::name"
      if (name[i - 1] == ')')
        paren++;
      else if (name[i - 1] == '(')
        paren--;
      else if (!(paren > 0 || isIdentifierBody(name[i - 1]) ||
                 name[i - 1] == ':'))
        break;
    } @_81_
    def.qual_name_offset = i;
    def.detailed_name = intern(name);
  } @_78_

  void setVarName(const Decl *d, std::string_view short_name,
                  std::string_view qualified, IndexVar::Def &def) { @_82_
    QualType t;
    const Expr *init = nullptr;
    bool deduced = false;
    if (auto *vd = dyn_cast<VarDecl>(d)) { @_83_
      t = vd->getType();
      init = vd->getAnyInitializer();
      def.storage = vd->getStorageClass();
    } else if (auto *fd = dyn_cast<FieldDecl>(d)) {
      t = fd->getType();
      init = fd->getInClassInitializer();
    } else if (auto *bd = dyn_cast<BindingDecl>(d)) {
      t = bd->getType();
      deduced = true;
    } @_83_
    if (!t.isNull()) { @_84_
      if (t->getContainedDeducedType()) { @_85_
        deduced = true;
      } else if (auto *dt = dyn_cast<DecltypeType>(t)) {
        // decltype(y) x;
        while (dt && !dt->getUnderlyingType().isNull()) { @_86_
          t = dt->getUnderlyingType();
          dt = dyn_cast<DecltypeType>(t);
        } @_86_
        deduced = true;
      } @_85_
    } @_84_
    if (!t.isNull() && deduced) { @_87_
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
    } @_87_
    if (init) { @_88_
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
    } @_88_
  } @_82_

  static int getFileLID(IndexFile *db, SourceManager &sm, FileID fid) { @_89_
    auto [it, inserted] = db->uid2lid_and_path.try_emplace(fid);
    if (inserted) { @_90_
      const FileEntry *fe = sm.getFileEntryForID(fid);
      if (!fe) { @_91_
        it->second.first = -1;
        return -1;
      } @_91_
      it->second.first = db->uid2lid_and_path.size() - 1;
      it->second.second = pathFromFileEntry(*fe);
    } @_90_
    return it->second.first;
  } @_89_

  void addMacroUse(IndexFile *db, SourceManager &sm, Usr usr, Kind kind,
                   SourceLocation sl) const { @_92_
    FileID fid = sm.getFileID(sl);
    int lid = getFileLID(db, sm, fid);
    if (lid < 0)
      return;
    Range spell = fromTokenRange(sm, ctx->getLangOpts(), SourceRange(sl, sl));
    Use use{{spell, Role::Dynamic}, lid};
    switch (kind) { @_93_
    case Kind::Func: @_94_
      db->toFunc(usr).uses.push_back(use);
      break; @_94_
    case Kind::Type: @_95_
      db->toType(usr).uses.push_back(use);
      break; @_95_
    case Kind::Var: @_96_
      db->toVar(usr).uses.push_back(use);
      break;
    default:
      llvm_unreachable(""); @_96_
    } @_93_
  } @_92_

  void collectRecordMembers(IndexType &type, const RecordDecl *rd) { @_97_
    SmallVector<std::pair<const RecordDecl *, int>, 2> stack{{rd, 0}};
    llvm::DenseSet<const RecordDecl *> seen;
    seen.insert(rd);
    while (stack.size()) { @_98_
      int offset;
      std::tie(rd, offset) = stack.back();
      stack.pop_back();
      if (!rd->isCompleteDefinition() || rd->isDependentType() ||
          rd->isInvalidDecl() || !validateRecord(rd))
        offset = -1;
      for (FieldDecl *fd : rd->fields()) { @_99_
        int offset1 = offset < 0 ? -1 : int(offset + ctx->getFieldOffset(fd));
        if (fd->getIdentifier())
          type.def.vars.emplace_back(getUsr(fd), offset1);
        else if (const auto *rt1 = fd->getType()->getAs<RecordType>()) { @_100_
          if (const RecordDecl *rd1 = rt1->getDecl())
            if (seen.insert(rd1).second)
              stack.push_back({rd1, offset1});
        } @_100_
      } @_99_
    } @_98_
  } @_97_

public:
  IndexDataConsumer(IndexParam &param) : param(param) {}
  void initialize(ASTContext &ctx) override { this->ctx = param.ctx = &ctx; }
#if LLVM_VERSION_MAJOR < 10 // llvmorg-10-init-12036-g3b9715cb219 @_101_
# define handleDeclOccurrence handleDeclOccurence
#endif @_101_
  bool handleDeclOccurrence(const Decl *d, index::SymbolRoleSet roles,
                            ArrayRef<index::SymbolRelation> relations,
                            SourceLocation src_loc,
                            ASTNodeInfo ast_node) override { @_102_
    if (!param.no_linkage) { @_103_
      if (auto *nd = dyn_cast<NamedDecl>(d); nd && nd->hasLinkage())
        ;
      else
        return true;
    } @_103_
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
    if (g_config->index.multiVersion && param.useMultiVersion(fid)) { @_104_
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
    } @_104_

    // spell, extent, comments use OrigD while most others use adjusted |D|.
    const Decl *origD = ast_node.OrigD;
    const DeclContext *sem_dc = origD->getDeclContext()->getRedeclContext();
    const DeclContext *lex_dc = ast_node.ContainerDC->getRedeclContext();
    { @_105_
      const NamespaceDecl *nd;
      while ((nd = dyn_cast<NamespaceDecl>(cast<Decl>(sem_dc))) &&
             nd->isAnonymousNamespace())
        sem_dc = nd->getDeclContext()->getRedeclContext();
      while ((nd = dyn_cast<NamespaceDecl>(cast<Decl>(lex_dc))) &&
             nd->isAnonymousNamespace())
        lex_dc = nd->getDeclContext()->getRedeclContext();
    } @_105_
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
      switch (d->getKind()) { @_106_
      case Decl::CXXConversion: // *operator* int => *operator int*
      case Decl::CXXDestructor: // *~*A => *~A*
      case Decl::CXXMethod:     // *operator*= => *operator=*
      case Decl::Function:      // operator delete @_107_
        if (src_loc.isFileID()) { @_108_
          SourceRange sr =
              cast<FunctionDecl>(origD)->getNameInfo().getSourceRange();
          if (sr.getEnd().isFileID())
            loc = fromTokenRange(sm, lang, sr);
        } @_108_
        break;
      default:
        break; @_107_
      } @_106_
    else { @_109_
      // e.g. typedef Foo<int> gg; => Foo has an unadjusted `D`
      const Decl *d1 = getAdjustedDecl(d);
      if (d1 && d1 != d)
        d = d1;
    } @_109_

    IndexParam::DeclInfo *info;
    Usr usr = getUsr(d, &info);

    auto do_def_decl = [&](auto *entity) { @_110_
      Use use{{loc, role}, lid};
      if (is_def) { @_111_
        SourceRange sr = origD->getSourceRange();
        entity->def.spell = {use, @_112_
                             fromTokenRangeDefaulted(sm, lang, sr, fid, loc)}; @_112_
        entity->def.parent_kind = SymbolKind::File;
        getKind(cast<Decl>(sem_dc), entity->def.parent_kind);
      } else if (is_decl) {
        SourceRange sr = origD->getSourceRange();
        entity->declarations.push_back(
            {use, fromTokenRangeDefaulted(sm, lang, sr, fid, loc)});
      } else {
        entity->uses.push_back(use);
        return;
      } @_111_
      if (entity->def.comments[0] == '\0' && g_config->index.comments)
        entity->def.comments = intern(getComment(origD));
    }; @_110_
    switch (kind) { @_113_
    case Kind::Invalid: @_114_
      if (ls_kind == SymbolKind::Unknown)
        LOG_S(INFO) << "Unhandled " << int(d->getKind()) << " "
                    << info->qualified << " in " << db->path << ":"
                    << (loc.start.line + 1) << ":" << (loc.start.column + 1);
      return true; @_114_
    case Kind::File: @_115_
      return true; @_115_
    case Kind::Func: @_116_
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
      if (is_def || is_decl) { @_117_
        const Decl *dc = cast<Decl>(sem_dc);
        if (getKind(dc, ls_kind) == Kind::Type)
          db->toType(getUsr(dc)).def.funcs.push_back(usr);
      } else {
        const Decl *dc = cast<Decl>(lex_dc);
        if (getKind(dc, ls_kind) == Kind::Func)
          db->toFunc(getUsr(dc))
              .def.callees.push_back({loc, usr, Kind::Func, role});
      } @_117_
      break; @_116_
    case Kind::Type: @_118_
      type = &db->toType(usr);
      type->def.kind = ls_kind;
      do_def_decl(type);
      if (spell != src_loc)
        addMacroUse(db, sm, usr, Kind::Type, spell);
      if ((is_def || type->def.detailed_name[0] == '\0') &&
          info->short_name.size()) { @_119_
        if (d->getKind() == Decl::TemplateTypeParm)
          type->def.detailed_name = intern(info->short_name);
        else
          // OrigD may be detailed, e.g. "struct D : B {}"
          setName(origD, info->short_name, info->qualified, type->def);
      } @_119_
      if (is_def || is_decl) { @_120_
        const Decl *dc = cast<Decl>(sem_dc);
        if (getKind(dc, ls_kind) == Kind::Type)
          db->toType(getUsr(dc)).def.types.push_back(usr);
      } @_120_
      break; @_118_
    case Kind::Var: @_121_
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
      if (is_def || is_decl) { @_122_
        const Decl *dc = cast<Decl>(sem_dc);
        Kind kind = getKind(dc, var->def.parent_kind);
        if (kind == Kind::Func)
          db->toFunc(getUsr(dc)).def.vars.push_back(usr);
        else if (kind == Kind::Type && !isa<RecordDecl>(sem_dc))
          db->toType(getUsr(dc)).def.vars.emplace_back(usr, -1);
        if (!t.isNull()) { @_123_
          if (auto *bt = t->getAs<BuiltinType>()) { @_124_
            Usr usr1 = static_cast<Usr>(bt->getKind());
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(d))
              db->toType(usr1).instances.push_back(usr);
          } else if (const Decl *d1 = getAdjustedDecl(getTypeDecl(t))) {
#if LLVM_VERSION_MAJOR < 9 @_125_
            if (isa<TemplateTypeParmDecl>(d1)) { @_126_
              // e.g. TemplateTypeParmDecl is not handled by
              // handleDeclOccurence.
              SourceRange sr1 = d1->getSourceRange();
              if (sm.getFileID(sr1.getBegin()) == fid) { @_127_
                IndexParam::DeclInfo *info1;
                Usr usr1 = getUsr(d1, &info1);
                IndexType &type1 = db->toType(usr1);
                SourceLocation sl1 = d1->getLocation();
                type1.def.spell = { @_128_
                    Use{{fromTokenRange(sm, lang, {sl1, sl1}), Role::Definition}, @_129_
                        lid}, @_129_
                    fromTokenRange(sm, lang, sr1)}; @_128_
                type1.def.detailed_name = intern(info1->short_name);
                type1.def.short_name_size = int16_t(info1->short_name.size());
                type1.def.kind = SymbolKind::TypeParameter;
                type1.def.parent_kind = SymbolKind::Class;
                var->def.type = usr1;
                type1.instances.push_back(usr);
                break;
              } @_127_
            } @_126_
#endif @_125_

            IndexParam::DeclInfo *info1;
            Usr usr1 = getUsr(d1, &info1);
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(d))
              db->toType(usr1).instances.push_back(usr);
          } @_124_
        } @_123_
      } else if (!var->def.spell && var->declarations.empty()) {
        // e.g. lambda parameter
        SourceLocation l = d->getLocation();
        if (sm.getFileID(l) == fid) { @_130_
          var->def.spell = { @_131_
              Use{{fromTokenRange(sm, lang, {l, l}), Role::Definition}, lid},
              fromTokenRange(sm, lang, d->getSourceRange())}; @_131_
          var->def.parent_kind = SymbolKind::Method;
        } @_130_
      } @_122_
      break; @_121_
    } @_113_

    switch (d->getKind()) { @_132_
    case Decl::Namespace: @_133_
      if (d->isFirstDecl()) { @_134_
        auto *nd = cast<NamespaceDecl>(d);
        auto *nd1 = cast<Decl>(nd->getParent());
        if (isa<NamespaceDecl>(nd1)) { @_135_
          Usr usr1 = getUsr(nd1);
          type->def.bases.push_back(usr1);
          db->toType(usr1).derived.push_back(usr);
        } @_135_
      } @_134_
      break; @_133_
    case Decl::NamespaceAlias: { @_136_ @_137_
      auto *nad = cast<NamespaceAliasDecl>(d);
      if (const NamespaceDecl *nd = nad->getNamespace()) { @_138_
        Usr usr1 = getUsr(nd);
        type->def.alias_of = usr1;
        (void)db->toType(usr1);
      } @_138_
      break;
    } @_136_ @_137_
    case Decl::CXXRecord: @_139_
      if (is_def) { @_140_
        auto *rd = dyn_cast<CXXRecordDecl>(d);
        if (rd && rd->hasDefinition())
          for (const CXXBaseSpecifier &base : rd->bases())
            if (const Decl *baseD =
                    getAdjustedDecl(getTypeDecl(base.getType()))) { @_141_
              Usr usr1 = getUsr(baseD);
              type->def.bases.push_back(usr1);
              db->toType(usr1).derived.push_back(usr);
            } @_141_
      } @_140_
      [[fallthrough]]; @_139_
    case Decl::Enum:
    case Decl::Record: @_142_
      if (auto *tag_d = dyn_cast<TagDecl>(d)) { @_143_
        if (type->def.detailed_name[0] == '\0' && info->short_name.empty()) { @_144_
          StringRef tag;
          switch (tag_d->getTagKind()) { @_145_
          case TTK_Struct: @_146_
            tag = "struct";
            break; @_146_
          case TTK_Interface: @_147_
            tag = "__interface";
            break; @_147_
          case TTK_Union: @_148_
            tag = "union";
            break; @_148_
          case TTK_Class: @_149_
            tag = "class";
            break; @_149_
          case TTK_Enum: @_150_
            tag = "enum";
            break; @_150_
          } @_145_
          if (TypedefNameDecl *td = tag_d->getTypedefNameForAnonDecl()) { @_151_
            StringRef name = td->getName();
            std::string detailed = ("anon " + tag + " " + name).str();
            type->def.detailed_name = intern(detailed);
            type->def.short_name_size = detailed.size();
          } else {
            std::string name = ("anon " + tag).str();
            type->def.detailed_name = intern(name);
            type->def.short_name_size = name.size();
          } @_151_
        } @_144_
        if (is_def && !isa<EnumDecl>(d))
          if (auto *ord = dyn_cast<RecordDecl>(origD))
            collectRecordMembers(*type, ord);
      } @_143_
      break; @_142_
    case Decl::ClassTemplateSpecialization:
    case Decl::ClassTemplatePartialSpecialization: @_152_
      type->def.kind = SymbolKind::Class;
      if (is_def) { @_153_
        if (auto *ord = dyn_cast<RecordDecl>(origD))
          collectRecordMembers(*type, ord);
        if (auto *rd = dyn_cast<CXXRecordDecl>(d)) { @_154_
          Decl *d1 = nullptr;
          if (auto *sd = dyn_cast<ClassTemplatePartialSpecializationDecl>(rd))
            d1 = sd->getSpecializedTemplate();
          else if (auto *sd = dyn_cast<ClassTemplateSpecializationDecl>(rd)) { @_155_
            llvm::PointerUnion<ClassTemplateDecl *,
                               ClassTemplatePartialSpecializationDecl *>
                result = sd->getSpecializedTemplateOrPartial();
            if (result.is<ClassTemplateDecl *>())
              d1 = result.get<ClassTemplateDecl *>();
            else
              d1 = result.get<ClassTemplatePartialSpecializationDecl *>();

          } else @_155_
            d1 = rd->getInstantiatedFromMemberClass();
          if (d1) { @_156_
            Usr usr1 = getUsr(d1);
            type->def.bases.push_back(usr1);
            db->toType(usr1).derived.push_back(usr);
          } @_156_
        } @_154_
      } @_153_
      break; @_152_
    case Decl::TypeAlias:
    case Decl::Typedef:
    case Decl::UnresolvedUsingTypename: @_157_
      if (auto *td = dyn_cast<TypedefNameDecl>(d)) { @_158_
        bool specialization = false;
        QualType t = td->getUnderlyingType();
        if (const Decl *d1 = getAdjustedDecl(getTypeDecl(t, &specialization))) { @_159_
          Usr usr1 = getUsr(d1);
          IndexType &type1 = db->toType(usr1);
          type->def.alias_of = usr1;
          // Not visited template<class T> struct B {typedef A<T> t;};
          if (specialization) { @_160_
            const TypeSourceInfo *tsi = td->getTypeSourceInfo();
            SourceLocation l1 = tsi->getTypeLoc().getBeginLoc();
            if (sm.getFileID(l1) == fid)
              type1.uses.push_back(
                  {{fromTokenRange(sm, lang, {l1, l1}), Role::Reference}, lid});
          } @_160_
        } @_159_
      } @_158_
      break; @_157_
    case Decl::CXXMethod: @_161_
      if (is_def || is_decl) { @_162_
        if (auto *nd = dyn_cast<NamedDecl>(d)) { @_163_
          SmallVector<const NamedDecl *, 8> overDecls;
          ctx->getOverriddenMethods(nd, overDecls);
          for (const auto *nd1 : overDecls) { @_164_
            Usr usr1 = getUsr(nd1);
            func->def.bases.push_back(usr1);
            db->toFunc(usr1).derived.push_back(usr);
          } @_164_
        } @_163_
      } @_162_
      break; @_161_
    case Decl::EnumConstant: @_165_
      if (is_def && strchr(var->def.detailed_name, '=') == nullptr) { @_166_
        auto *ecd = cast<EnumConstantDecl>(d);
        const auto &val = ecd->getInitVal();
        std::string init =
            " = " + (val.isSigned() ? std::to_string(val.getSExtValue())
                                    : std::to_string(val.getZExtValue()));
        var->def.hover = intern(var->def.detailed_name + init);
      } @_166_
      break;
    default:
      break; @_165_
    } @_132_
    return true;
  } @_102_
}; @_68_

class IndexPPCallbacks : public PPCallbacks { @_167_
  SourceManager &sm;
  IndexParam &param;

  std::pair<StringRef, Usr> getMacro(const Token &tok) const { @_168_
    StringRef name = tok.getIdentifierInfo()->getName();
    SmallString<256> usr("@macro@");
    usr += name;
    return {name, hashUsr(usr)};
  } @_168_

public:
  IndexPPCallbacks(SourceManager &sm, IndexParam &param)
      : sm(sm), param(param) {}
  void FileChanged(SourceLocation sl, FileChangeReason reason,
                   SrcMgr::CharacteristicKind, FileID) override { @_169_
    if (reason == FileChangeReason::EnterFile)
      (void)param.consumeFile(sm.getFileID(sl));
  } @_169_
  void InclusionDirective(SourceLocation hashLoc, const Token &tok,
                          StringRef included, bool isAngled,
                          CharSourceRange filenameRange, const FileEntry *file,
                          StringRef searchPath, StringRef relativePath,
                          const Module *imported,
                          SrcMgr::CharacteristicKind fileType) override { @_170_
    if (!file)
      return;
    auto spell = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                     filenameRange, nullptr);
    FileID fid = sm.getFileID(filenameRange.getBegin());
    if (IndexFile *db = param.consumeFile(fid)) { @_171_
      std::string path = pathFromFileEntry(*file);
      if (path.size())
        db->includes.push_back({spell.start.line, intern(path)});
    } @_171_
  } @_170_
  void MacroDefined(const Token &tok, const MacroDirective *md) override { @_172_
    const LangOptions &lang = param.ctx->getLangOpts();
    SourceLocation sl = md->getLocation();
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) { @_173_
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
      if (var.def.detailed_name[0] == '\0') { @_174_
        var.def.detailed_name = intern(name);
        var.def.short_name_size = name.size();
        StringRef buf = getSourceInRange(sm, lang, sr);
        var.def.hover =
            intern(buf.count('\n') <= g_config->index.maxInitializerLines - 1
                       ? Twine("#define ", getSourceInRange(sm, lang, sr)).str()
                       : Twine("#define ", name).str());
      } @_174_
    } @_173_
  } @_172_
  void MacroExpands(const Token &tok, const MacroDefinition &, SourceRange sr,
                    const MacroArgs *) override { @_175_
    SourceLocation sl = sm.getSpellingLoc(sr.getBegin());
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) { @_176_
      IndexVar &var = db->toVar(getMacro(tok).second);
      var.uses.push_back(
          {{fromTokenRange(sm, param.ctx->getLangOpts(), {sl, sl}, nullptr), @_177_ @_178_
            Role::Dynamic}}); @_177_ @_178_
    } @_176_
  } @_175_
  void MacroUndefined(const Token &tok, const MacroDefinition &md,
                      const MacroDirective *ud) override { @_179_
    if (ud) { @_180_
      SourceLocation sl = ud->getLocation();
      MacroExpands(tok, md, {sl, sl}, nullptr);
    } @_180_
  } @_179_
  void SourceRangeSkipped(SourceRange sr, SourceLocation) override { @_181_
    Range range = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                      CharSourceRange::getCharRange(sr));
    FileID fid = sm.getFileID(sr.getBegin());
    if (fid.isValid())
      if (IndexFile *db = param.consumeFile(fid))
        db->skipped_ranges.push_back(range);
  } @_181_
}; @_167_

class IndexFrontendAction : public ASTFrontendAction { @_182_
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
                                                 StringRef inFile) override { @_183_
    class SkipProcessed : public ASTConsumer { @_184_
      IndexParam &param;
      const ASTContext *ctx = nullptr;

    public:
      SkipProcessed(IndexParam &param) : param(param) {}
      void Initialize(ASTContext &ctx) override { this->ctx = &ctx; }
      bool shouldSkipFunctionBody(Decl *d) override { @_185_
        const SourceManager &sm = ctx->getSourceManager();
        FileID fid = sm.getFileID(sm.getExpansionLoc(d->getLocation()));
        return !(g_config->index.multiVersion && param.useMultiVersion(fid)) &&
               !param.consumeFile(fid);
      } @_185_
    }; @_184_

    std::shared_ptr<Preprocessor> pp = ci.getPreprocessorPtr();
    pp->addPPCallbacks(
        std::make_unique<IndexPPCallbacks>(pp->getSourceManager(), param));
    std::vector<std::unique_ptr<ASTConsumer>> consumers;
    consumers.push_back(std::make_unique<SkipProcessed>(param));
#if LLVM_VERSION_MAJOR >= 10 // rC370337 @_186_
    consumers.push_back(index::createIndexingASTConsumer(
        dataConsumer, indexOpts, std::move(pp)));
#endif @_186_
    return std::make_unique<MultiplexConsumer>(std::move(consumers));
  } @_183_
}; @_182_
} // namespace @_1_

const int IndexFile::kMajorVersion = 21;
const int IndexFile::kMinorVersion = 0;

IndexFile::IndexFile(const std::string &path, const std::string &contents,
                     bool no_linkage)
    : path(path), no_linkage(no_linkage), file_contents(contents) {}

IndexFunc &IndexFile::toFunc(Usr usr) { @_187_
  auto [it, inserted] = usr2func.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_187_

IndexType &IndexFile::toType(Usr usr) { @_188_
  auto [it, inserted] = usr2type.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_188_

IndexVar &IndexFile::toVar(Usr usr) { @_189_
  auto [it, inserted] = usr2var.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
} @_189_

std::string IndexFile::toString() { @_190_
  return ccls::serialize(SerializeFormat::Json, *this);
} @_190_

template <typename T> void uniquify(std::vector<T> &a) { @_191_
  std::unordered_set<T> seen;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); i++)
    if (seen.insert(a[i]).second)
      a[n++] = a[i];
  a.resize(n);
} @_191_

namespace idx { @_192_
void init() { @_193_
  multiVersionMatcher = new GroupMatch(g_config->index.multiVersionWhitelist,
                                       g_config->index.multiVersionBlacklist);
} @_193_

std::vector<std::unique_ptr<IndexFile>>
index(SemaManager *manager, WorkingFiles *wfiles, VFS *vfs,
      const std::string &opt_wdir, const std::string &main,
      const std::vector<const char *> &args,
      const std::vector<std::pair<std::string, std::string>> &remapped,
      bool no_linkage, bool &ok) { @_194_
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
    for (auto &[filename, content] : remapped) { @_195_
      bufs.push_back(llvm::MemoryBuffer::getMemBuffer(content));
      ci->getPreprocessorOpts().addRemappedFile(filename, bufs.back().get());
    } @_195_

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
#if LLVM_VERSION_MAJOR >= 9 // rC357037 @_196_
  clang->createFileManager(fs); @_196_
#else @_197_
  clang->setVirtualFileSystem(fs);
  clang->createFileManager();
#endif @_197_
  clang->setSourceManager(new SourceManager(clang->getDiagnostics(),
                                            clang->getFileManager(), true));

  IndexParam param(*vfs, no_linkage);

  index::IndexingOptions indexOpts;
  indexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  if (no_linkage) { @_198_
    indexOpts.IndexFunctionLocals = true;
    indexOpts.IndexImplicitInstantiation = true;
#if LLVM_VERSION_MAJOR >= 9 @_199_

    indexOpts.IndexParametersInDeclarations =
        g_config->index.parametersInDeclarations;
    indexOpts.IndexTemplateParameters = true;
#endif @_199_
  } @_198_

#if LLVM_VERSION_MAJOR >= 10 // rC370337 @_200_
  auto action = std::make_unique<IndexFrontendAction>(
      std::make_shared<IndexDataConsumer>(param), indexOpts, param); @_200_
#else @_201_
  auto dataConsumer = std::make_shared<IndexDataConsumer>(param);
  auto action = createIndexingAction(
      dataConsumer, indexOpts,
      std::make_unique<IndexFrontendAction>(dataConsumer, indexOpts, param));
#endif @_201_

  std::string reason;
  { @_202_
    llvm::CrashRecoveryContext crc;
    auto parse = [&]() { @_203_
      if (!action->BeginSourceFile(*clang, clang->getFrontendOpts().Inputs[0]))
        return;
#if LLVM_VERSION_MAJOR >= 9 // rL364464 @_204_
      if (llvm::Error e = action->Execute()) { @_205_
        reason = llvm::toString(std::move(e));
        return;
      } @_204_ @_205_
#else @_206_
      if (!action->Execute())
        return;
#endif @_206_
      action->EndSourceFile();
      ok = true;
    }; @_203_
    if (!crc.RunSafely(parse)) { @_207_
      LOG_S(ERROR) << "clang crashed for " << main;
      return {};
    } @_207_
  } @_202_
  if (!ok) { @_208_
    LOG_S(ERROR) << "failed to index " << main
                 << (reason.empty() ? "" : ": " + reason);
    return {};
  } @_208_

  std::vector<std::unique_ptr<IndexFile>> result;
  for (auto &it : param.uid2file) { @_209_
    if (!it.second.db)
      continue;
    std::unique_ptr<IndexFile> &entry = it.second.db;
    entry->import_file = main;
    entry->args = args;
    for (auto &[_, it] : entry->uid2lid_and_path)
      if (it.first >= 0)
        entry->lid2path.emplace_back(it.first, std::move(it.second));
    entry->uid2lid_and_path.clear();
    for (auto &it : entry->usr2func) { @_210_
      // e.g. declaration + out-of-line definition
      uniquify(it.second.derived);
      uniquify(it.second.uses);
    } @_210_
    for (auto &it : entry->usr2type) { @_211_
      uniquify(it.second.derived);
      uniquify(it.second.uses);
      // e.g. declaration + out-of-line definition
      uniquify(it.second.def.bases);
      uniquify(it.second.def.funcs);
    } @_211_
    for (auto &it : entry->usr2var)
      uniquify(it.second.uses);

    // Update dependencies for the file.
    for (auto &[_, file] : param.uid2file) { @_212_
      const std::string &path = file.path;
      if (path.empty())
        continue;
      if (path == entry->path)
        entry->mtime = file.mtime;
      else if (path != entry->import_file)
        entry->dependencies[llvm::CachedHashStringRef(intern(path))] =
            file.mtime;
    } @_212_
    result.push_back(std::move(entry));
  } @_209_

  return result;
} @_194_
} // namespace idx @_192_

void reflect(JsonReader &vis, SymbolRef &v) { @_213_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.usr = strtoull(s + 1, &s, 10);
  v.kind = static_cast<Kind>(strtol(s + 1, &s, 10));
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
} @_213_
void reflect(JsonReader &vis, Use &v) { @_214_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
} @_214_
void reflect(JsonReader &vis, DeclRef &v) { @_215_
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|') + 1;
  v.extent = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
} @_215_

void reflect(JsonWriter &vis, SymbolRef &v) { @_216_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%" PRIu64 "|%d|%d", v.range.toString().c_str(),
           v.usr, int(v.kind), int(v.role));
  std::string s(buf);
  reflect(vis, s);
} @_216_
void reflect(JsonWriter &vis, Use &v) { @_217_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%d|%d", v.range.toString().c_str(), int(v.role),
           v.file_id);
  std::string s(buf);
  reflect(vis, s);
} @_217_
void reflect(JsonWriter &vis, DeclRef &v) { @_218_
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%s|%d|%d", v.range.toString().c_str(),
           v.extent.toString().c_str(), int(v.role), v.file_id);
  std::string s(buf);
  reflect(vis, s);
} @_218_

void reflect(BinaryReader &vis, SymbolRef &v) { @_219_
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
} @_219_
void reflect(BinaryReader &vis, Use &v) { @_220_
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
} @_220_
void reflect(BinaryReader &vis, DeclRef &v) { @_221_
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
} @_221_

void reflect(BinaryWriter &vis, SymbolRef &v) { @_222_
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
} @_222_
void reflect(BinaryWriter &vis, Use &v) { @_223_
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
} @_223_
void reflect(BinaryWriter &vis, DeclRef &v) { @_224_
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
} @_224_
} // namespace ccls @_0_
