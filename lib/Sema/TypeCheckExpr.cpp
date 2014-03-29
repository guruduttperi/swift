//===--- TypeCheckExpr.cpp - Type Checking for Expressions ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for expressions, analysing an
// expression tree in post-order, bottom-up, from leaves up to the root.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Decl.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// Expression Semantic Analysis Routines
//===----------------------------------------------------------------------===//

void substituteInputSugarArgumentType(Type argTy,
                                      CanType resultTy,
                                      Type &resultSugarTy,
                                      bool &uniqueSugarTy) {
  // If we already failed finding a unique sugar, bail out.
  if (!uniqueSugarTy)
    return;
    
  if (TupleType *argTupleTy = argTy->getAs<TupleType>()) {
    // Recursively walk tuple arguments.
    for (auto &field : argTupleTy->getFields()) {
      substituteInputSugarArgumentType(field.getType(), resultTy,
                                       resultSugarTy, uniqueSugarTy);
      if (!uniqueSugarTy)
        return;
    }
  } else {
    if (argTy->getCanonicalType() == resultTy) {
      if (resultSugarTy) {
        // Make sure this argument's sugar is consistent with the sugar we
        // already found.
        if (argTy->isSpelledLike(resultSugarTy))
          return;
        uniqueSugarTy = false;
        return;
      } else {
        resultSugarTy = argTy;
      }
    }
  }
}

/// If the inputs to an apply expression use a consistent "sugar" type
/// (that is, a typealias or shorthand syntax) equivalent to the result type of
/// the function, set the result type of the expression to that sugar type.
Expr *TypeChecker::substituteInputSugarTypeForResult(ApplyExpr *E) {
  if (!E->getType() || E->getType()->is<ErrorType>())
    return E;
  
  Type argTy = E->getArg()->getType();
  
  CanType resultTy = E->getFn()->getType()->castTo<FunctionType>()->getResult()
    ->getCanonicalType();
  
  Type resultSugarTy; // null if no sugar found, set when sugar found
  bool uniqueSugarTy = true; // true if a unique sugar mapping found
  
  substituteInputSugarArgumentType(argTy, resultTy,
                                   resultSugarTy, uniqueSugarTy);
  
  if (resultSugarTy && uniqueSugarTy)
    E->setType(resultSugarTy);

  return E;
}

/// Is the given expression a valid thing to use as the injection
/// function from the data for a newly-allocated array into the
/// given slice type?
Expr *TypeChecker::buildArrayInjectionFnRef(DeclContext *dc,
                                            ArraySliceType *sliceType,
                                            Type lenTy, SourceLoc Loc) {
  // Build the expression "Array<T>".
  Expr *sliceTypeRef =
    new (Context) MetatypeExpr(nullptr, Loc, MetatypeType::get(sliceType));

  // Build the expression "Array<T>.convertFromHeapArray".
  Expr *injectionFn = new (Context) UnresolvedDotExpr(
                                      sliceTypeRef, Loc,
                                  Context.getIdentifier("convertFromHeapArray"),
                                      Loc, /*Implicit=*/true);
  if (typeCheckExpressionShallow(injectionFn, dc))
    return nullptr;

  // The input is a tuple type:
  TupleTypeElt argTypes[3] = {
    // The first element is Builtin.RawPointer.
    // FIXME: this should probably be UnsafePointer<T>.
    Context.TheRawPointerType,

    // The second element is the owner pointer, Builtin.ObjectPointer.
    Context.TheObjectPointerType,

    // The third element is the bound type.  Maybe this should be a
    // target-specific size_t type?
    lenTy
  };

  Type input = TupleType::get(argTypes, Context);

  // The result is just the slice type.
  Type result = sliceType;

  FunctionType *fnTy = FunctionType::get(input, result);

  // FIXME: this produces terrible diagnostics.
  if (convertToType(injectionFn, fnTy, dc))
    return nullptr;

  return injectionFn;
}

/// getInfixData - If the specified expression is an infix binary
/// operator, return its infix operator attributes.
static InfixData getInfixData(TypeChecker &TC, DeclContext *DC, Expr *E) {
  if (auto *ifExpr = dyn_cast<IfExpr>(E)) {
    // Ternary has fixed precedence.
    assert(!ifExpr->isFolded() && "already folded if expr in sequence?!");
    (void)ifExpr;
    return InfixData(100, Associativity::Right);

  } else if (auto *assign = dyn_cast<AssignExpr>(E)) {
    // Assignment has fixed precedence.
    assert(!assign->isFolded() && "already folded assign expr in sequence?!");
    (void)assign;
    return InfixData(90, Associativity::Right);

  } else if (auto *as = dyn_cast<ExplicitCastExpr>(E)) {
    // 'as' and 'is' casts have fixed precedence.
    assert(!as->isFolded() && "already folded 'as' expr in sequence?!");
    (void)as;
    return InfixData(95, Associativity::None);

  } else if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    SourceFile *SF = DC->getParentSourceFile();
    Identifier name = DRE->getDecl()->getName();
    if (InfixOperatorDecl *op = SF->lookupInfixOperator(name, E->getLoc()))
      return op->getInfixData();

  } else if (OverloadedDeclRefExpr *OO = dyn_cast<OverloadedDeclRefExpr>(E)) {
    SourceFile *SF = DC->getParentSourceFile();
    Identifier name = OO->getDecls()[0]->getName();
    if (InfixOperatorDecl *op = SF->lookupInfixOperator(name, E->getLoc()))
      return op->getInfixData();
  }
  
  TC.diagnose(E->getLoc(), diag::unknown_binop);
  // Recover with an infinite-precedence left-associative operator.
  return InfixData((unsigned char)~0U, Associativity::Left);
}

static Expr *makeBinOp(TypeChecker &TC, Expr *Op, Expr *LHS, Expr *RHS) {
  if (!LHS || !RHS)
    return nullptr;
  
  if (auto *ifExpr = dyn_cast<IfExpr>(Op)) {
    // Resolve the ternary expression.
    assert(!ifExpr->isFolded() && "already folded if expr in sequence?!");
    ifExpr->setCondExpr(LHS);
    ifExpr->setElseExpr(RHS);
    return ifExpr;
  }

  if (auto *assign = dyn_cast<AssignExpr>(Op)) {
    // Resolve the assignment expression.
    assert(!assign->isFolded() && "already folded assign expr in sequence?!");
    assign->setDest(LHS);
    assign->setSrc(RHS);
    return assign;
  }
  
  if (auto *as = dyn_cast<ExplicitCastExpr>(Op)) {
    // Resolve the 'as' or 'is' expression.
    assert(!as->isFolded() && "already folded 'as' expr in sequence?!");
    assert(RHS == as && "'as' with non-type RHS?!");
    as->setSubExpr(LHS);
    
    // If the cast was forced, add the ForceValueExpr here.
    auto forceLoc = as->getForceLoc();
    if (forceLoc.isValid()) {
      as->setForceLoc(SourceLoc());
      return new (TC.Context) ForceValueExpr(as, forceLoc);
    }
    
    return as;
  }
  
  // Build the argument to the operation.
  Expr *ArgElts[] = { LHS, RHS };
  auto ArgElts2 = TC.Context.AllocateCopy(MutableArrayRef<Expr*>(ArgElts));
  TupleExpr *Arg = new (TC.Context) TupleExpr(SourceLoc(), 
                                              ArgElts2, 0, SourceLoc(),
                                              /*hasTrailingClosure=*/false,
                                        LHS->isImplicit() && RHS->isImplicit());

  // Build the operation.
  return new (TC.Context) BinaryExpr(Op, Arg, Op->isImplicit());
}

/// foldSequence - Take a sequence of expressions and fold a prefix of
/// it into a tree of BinaryExprs using precedence parsing.
static Expr *foldSequence(TypeChecker &TC, DeclContext *DC,
                          Expr *LHS,
                          ArrayRef<Expr*> &S,
                          unsigned MinPrecedence) {
  // Invariant: S is even-sized.
  // Invariant: All elements at even indices are operator references.
  assert(!S.empty());
  assert((S.size() & 1) == 0);
  
  struct Op {
    Expr *op;
    InfixData infixData;
    
    explicit operator bool() const { return op; }
  };
  
  /// Get the operator, if appropriate to this pass.
  auto getNextOperator = [&]() -> Op {
    Expr *op = S[0];

    // If the operator's precedence is lower than the minimum, stop here.
    InfixData opInfo = getInfixData(TC, DC, op);
    if (opInfo.getPrecedence() < MinPrecedence) return {nullptr, {}};
    return {op, opInfo};
  };

  // Extract out the first operator.
  Op Op1 = getNextOperator();
  if (!Op1) return LHS;
  
  // We will definitely be consuming at least one operator.
  // Pull out the prospective RHS and slice off the first two elements.
  Expr *RHS = S[1];
  S = S.slice(2);
  
  while (!S.empty()) {
    assert(!S.empty());
    assert((S.size() & 1) == 0);
    assert(Op1.infixData.getPrecedence() >= MinPrecedence);
    
    // If the operator is a cast operator, the RHS can't extend past the type
    // that's part of the cast production.
    if (isa<ExplicitCastExpr>(Op1.op)) {
      LHS = makeBinOp(TC, Op1.op, LHS, RHS);
      Op1 = getNextOperator();
      RHS = S[1];
      S = S.slice(2);
      continue;
    }
    
    // Pull out the next binary operator.
    Expr *Op2 = S[0];
  
    InfixData Op2Info = getInfixData(TC, DC, Op2);
    // If the second operator's precedence is lower than the min
    // precedence, break out of the loop.
    if (Op2Info.getPrecedence() < MinPrecedence) break;
    
    // If the first operator's precedence is higher than the second
    // operator's precedence, or they have matching precedence and are
    // both left-associative, fold LHS and RHS immediately.
    if (Op1.infixData.getPrecedence() > Op2Info.getPrecedence() ||
        (Op1.infixData == Op2Info && Op1.infixData.isLeftAssociative())) {
      LHS = makeBinOp(TC, Op1.op, LHS, RHS);
      Op1 = getNextOperator();
      assert(Op1 && "should get a valid operator here");
      RHS = S[1];
      S = S.slice(2);
      continue;
    }

    // If the first operator's precedence is lower than the second
    // operator's precedence, recursively fold all such
    // higher-precedence operators starting from this point, then
    // repeat.
    if (Op1.infixData.getPrecedence() < Op2Info.getPrecedence()) {
      RHS = foldSequence(TC, DC, RHS, S, Op1.infixData.getPrecedence() + 1);
      continue;
    }

    // If the first operator's precedence is the same as the second
    // operator's precedence, and they're both right-associative,
    // recursively fold operators starting from this point, then
    // immediately fold LHS and RHS.
    if (Op1.infixData == Op2Info && Op1.infixData.isRightAssociative()) {
      RHS = foldSequence(TC, DC, RHS, S, Op1.infixData.getPrecedence());
      LHS = makeBinOp(TC, Op1.op, LHS, RHS);

      // If we've drained the entire sequence, we're done.
      if (S.empty()) return LHS;

      // Otherwise, start all over with our new LHS.
      return foldSequence(TC, DC, LHS, S, MinPrecedence);
    }

    // If we ended up here, it's because we have two operators
    // with mismatched or no associativity.
    assert(Op1.infixData.getPrecedence() == Op2Info.getPrecedence());
    assert(Op1.infixData.getAssociativity() != Op2Info.getAssociativity()
           || Op1.infixData.isNonAssociative());

    if (Op1.infixData.isNonAssociative()) {
      // FIXME: QoI ranges
      TC.diagnose(Op1.op->getLoc(), diag::non_assoc_adjacent);
    } else if (Op2Info.isNonAssociative()) {
      TC.diagnose(Op2->getLoc(), diag::non_assoc_adjacent);
    } else {
      TC.diagnose(Op1.op->getLoc(), diag::incompatible_assoc);
    }
    
    // Recover by arbitrarily binding the first two.
    LHS = makeBinOp(TC, Op1.op, LHS, RHS);
    return foldSequence(TC, DC, LHS, S, MinPrecedence);
  }

  // Fold LHS and RHS together and declare completion.
  return makeBinOp(TC, Op1.op, LHS, RHS);
}

Type TypeChecker::getTypeOfRValue(ValueDecl *value, bool wantInterfaceType) {
  validateDecl(value);

  Type type;
  if (wantInterfaceType)
    type = value->getInterfaceType();
  else
    type = value->getType();

  // Look at the canonical type just for efficiency.  We won't
  // use this as the source of the result.
  CanType canType = type->getCanonicalType();

  // Uses of inout argument values are lvalues.
  if (auto iot = dyn_cast<InOutType>(canType))
    return iot->getObjectType();
  
  // Uses of values with lvalue type produce their rvalue.
  if (auto LV = dyn_cast<LValueType>(canType))
    return LV->getObjectType();
  
  // Turn @weak T into Optional<T>.
  if (isa<WeakStorageType>(canType)) {
    // On the one hand, we should probably use a better location than
    // the declaration's.  On the other hand, all these diagnostics
    // are "broken library" errors, so it should really never matter.

    auto refTy = type->castTo<ReferenceStorageType>()->getReferentType();
    auto optTy = getOptionalType(value->getLoc(), refTy);

    // If we can't create Optional<T>, use T instead of returning null.
    if (!optTy) return refTy;

    // Check that we can do intrinsic operations on Optional<T> before
    // returning.
    requireOptionalIntrinsics(value->getLoc());

    return optTy;
  }

  // Ignore @unowned qualification.
  if (isa<UnownedStorageType>(canType))
    return type->getReferenceStorageReferent();

  // No other transforms necessary.
  return type;
}

bool TypeChecker::requireOptionalIntrinsics(SourceLoc loc) {
  if (Context.hasOptionalIntrinsics(this)) return false;

  diagnose(loc, diag::optional_intrinsics_not_found);
  return true;
}


/// doesVarDeclMemberProduceLValue - Return true if a reference to the specified
/// VarDecl should produce an lvalue.  If present, baseType indicates the base
/// type of a member reference.
static bool doesVarDeclMemberProduceLValue(VarDecl *VD, Type baseType,
                                           DeclContext *UseDC) {
  // Get-only VarDecls always produce rvalues.
  if (!VD->isSettable(UseDC))
    return false;

  // If there is no base, or if the base isn't being used, it is settable.
  if (!baseType || VD->isStatic())
    return true;

  // If the base is a reference type, or if the base is mutable, then a
  // reference produces an lvalue.
  if (baseType->hasReferenceSemantics() || baseType->is<LValueType>())
    return true;

  // If the base is an rvalue, then we only produce an lvalue if the vardecl
  // is a computed property, whose setter is @!mutating.
  return VD->getSetter() && !VD->getSetter()->isMutating();
}

/// doesSubscriptDeclProduceLValue - Return true if a reference to the specified
/// SubscriptDecl should produce an lvalue.
static bool doesSubscriptDeclProduceLValue(SubscriptDecl *SD, Type baseType) {
  assert(baseType && "Subscript without a base expression?");
  // Get-only SubscriptDecls always produce rvalues.
  if (!SD->isSettable())
    return false;

  // If the base is a reference type, or if the base is mutable, then a
  // reference produces an lvalue.
  if (baseType->hasReferenceSemantics() || baseType->is<LValueType>())
    return true;

  // If the base is an rvalue, then we only produce an lvalue if both the getter
  // and setter are non-mutating.
  return !SD->getGetter()->isMutating() && !SD->getSetter()->isMutating();
}

Type TypeChecker::getUnopenedTypeOfReference(ValueDecl *value, Type baseType,
                                             DeclContext *UseDC,
                                             bool wantInterfaceType) {
  if (!value->hasType())
    typeCheckDecl(value, true);
  
  if (value->isInvalid())
    return ErrorType::get(Context);

  // Qualify 'var' declarations with an lvalue if the base is a reference or
  // has lvalue type.  If we are accessing a var member on an rvalue, it is
  // returned as an rvalue (and the access must be a load).
  if (auto *VD = dyn_cast<VarDecl>(value))
    if (doesVarDeclMemberProduceLValue(VD, baseType, UseDC))
      return LValueType::get(getTypeOfRValue(value, wantInterfaceType));

  Type requestedType = getTypeOfRValue(value, wantInterfaceType);

  // Check to see if the subscript-decl produces an lvalue.
  if (auto *SD = dyn_cast<SubscriptDecl>(value))
    if (doesSubscriptDeclProduceLValue(SD, baseType)) {
      // Subscript decls have function type.  For the purposes of later type
      // checker consumption, model this as returning an lvalue.
      auto *RFT = requestedType->castTo<FunctionType>();
      return FunctionType::get(RFT->getInput(),
                               LValueType::get(RFT->getResult()),
                               RFT->getExtInfo());
    }

  return requestedType;
}

Expr *TypeChecker::buildCheckedRefExpr(ValueDecl *value, DeclContext *UseDC,
                                       SourceLoc loc, bool Implicit) {
  auto type = getUnopenedTypeOfReference(value, Type(), UseDC);
  bool isDirectPropertyAccess = value->isUseFromContextDirect(UseDC);
  return new (Context) DeclRefExpr(value, loc, Implicit,
                                   isDirectPropertyAccess, type);
}

Expr *TypeChecker::buildRefExpr(ArrayRef<ValueDecl *> Decls,
                                DeclContext *UseDC, SourceLoc NameLoc,
                                bool Implicit, bool isSpecialized) {
  assert(!Decls.empty() && "Must have at least one declaration");

  if (Decls.size() == 1 && !isa<ProtocolDecl>(Decls[0]->getDeclContext())) {
    bool isDirectPropertyAccess = Decls[0]->isUseFromContextDirect(UseDC);
    auto result = new (Context) DeclRefExpr(Decls[0], NameLoc, Implicit,
                                            isDirectPropertyAccess);
    if (isSpecialized)
      result->setSpecialized();
    return result;
  }

  Decls = Context.AllocateCopy(Decls);
  auto result = new (Context) OverloadedDeclRefExpr(Decls, NameLoc, Implicit);
  result->setSpecialized(isSpecialized);
  return result;
}

static Type lookupGlobalType(TypeChecker &TC, DeclContext *dc, StringRef name) {
  UnqualifiedLookup lookup(TC.Context.getIdentifier(name),
                           dc->getModuleScopeContext(),
                           nullptr);
  TypeDecl *TD = lookup.getSingleTypeResult();
  if (!TD)
    return Type();
  TC.validateDecl(TD);
  return TD->getDeclaredType();
}

Type TypeChecker::getDefaultType(ProtocolDecl *protocol, DeclContext *dc) {
  Type *type = nullptr;
  const char *name = nullptr;

  // CharacterLiteralConvertible -> CharacterLiteralType
  if (protocol == getProtocol(SourceLoc(),
                              KnownProtocolKind::CharacterLiteralConvertible)) {
    type = &CharacterLiteralType;
    name = "CharacterLiteralType";
  }
  // StringLiteralConvertible -> StringLiteralType
  // StringInterpolationConvertible -> StringLiteralType
  else if (protocol == getProtocol(
                         SourceLoc(),
                         KnownProtocolKind::StringLiteralConvertible) ||
           protocol == getProtocol(
                         SourceLoc(),
                         KnownProtocolKind::StringInterpolationConvertible)) {
    type = &StringLiteralType;
    name = "StringLiteralType";
  }
  // IntegerLiteralConvertible -> IntegerLiteralType
  else if (protocol == getProtocol(
                         SourceLoc(),
                         KnownProtocolKind::IntegerLiteralConvertible)) {
    type = &IntLiteralType;
    name = "IntegerLiteralType";
  }
  // FloatLiteralConvertible -> FloatLiteralType
  else if (protocol == getProtocol(SourceLoc(),
                                   KnownProtocolKind::FloatLiteralConvertible)){
    type = &FloatLiteralType;
    name = "FloatLiteralType";
  }
  // ArrayLiteralConvertible -> Array
  else if (protocol == getProtocol(SourceLoc(),
                                   KnownProtocolKind::ArrayLiteralConvertible)){
    type = &ArrayLiteralType;
    name = "Array";
  }
  // DictionaryLiteralConvertible -> Dictionary
  else if (protocol == getProtocol(
                         SourceLoc(),
                         KnownProtocolKind::DictionaryLiteralConvertible)) {
    type = &DictionaryLiteralType;
    name = "Dictionary";
  }

  if (!type)
    return nullptr;

  // If we haven't found the type yet, look for it now.
  if (!*type) {
    *type = lookupGlobalType(*this, dc, name);

    if (!*type)
      *type = lookupGlobalType(*this, getStdlibModule(dc), name);

    // Strip off one level of sugar; we don't actually want to print
    // the name of the typealias itself anywhere.
    if (type && *type) {
      if (auto typeAlias = dyn_cast<NameAliasType>(type->getPointer()))
        *type = typeAlias->getDecl()->getUnderlyingType();
    }
  }

  return *type;
}

static NominalTypeDecl *getKnownPointerDecl(Module *Stdlib,
                                          ASTContext &Context,
                                          Optional<NominalTypeDecl*> &cacheSlot,
                                          StringRef name) {
  return cacheSlot.cache([&] {
    UnqualifiedLookup lookup(Context.getIdentifier(name),
                             Stdlib, nullptr);
    return cast_or_null<NominalTypeDecl>(lookup.getSingleTypeResult());
  });
}

NominalTypeDecl *TypeChecker::getUnsafePointerDecl(const DeclContext *DC) {
  return getKnownPointerDecl(getStdlibModule(DC), Context,
                             UnsafePointerDecl, "UnsafePointer");
}

NominalTypeDecl *TypeChecker::getCConstPointerDecl(const DeclContext *DC) {
  return getKnownPointerDecl(getStdlibModule(DC), Context,
                             CConstPointerDecl, "CConstPointer");
}

NominalTypeDecl *TypeChecker::getCMutablePointerDecl(const DeclContext *DC) {
  return getKnownPointerDecl(getStdlibModule(DC), Context,
                             CMutablePointerDecl, "CMutablePointer");
}

Expr *TypeChecker::foldSequence(SequenceExpr *expr, DeclContext *dc) {
  ArrayRef<Expr*> Elts = expr->getElements();
  assert(Elts.size() > 1 && "inadequate number of elements in sequence");
  assert((Elts.size() & 1) == 1 && "even number of elements in sequence");

  Expr *LHS = Elts[0];
  Elts = Elts.slice(1);

  Expr *Result = ::foldSequence(*this, dc, LHS, Elts, /*min precedence*/ 0);
  assert(Elts.empty());
  return Result;
}

namespace {
  class FindCapturedVars : public ASTWalker {
    llvm::SetVector<ValueDecl*> &captures;
    DeclContext *CurExprAsDC;

  public:
    FindCapturedVars(llvm::SetVector<ValueDecl*> &captures,
                     AnyFunctionRef AFR)
        : captures(captures), CurExprAsDC(AFR.getAsDeclContext()) {}

    void doWalk(Expr *E) {
      E->walk(*this);
    }
    void doWalk(Stmt *S) {
      S->walk(*this);
    }

    std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
      if (auto *DRE = dyn_cast<DeclRefExpr>(E))
        return walkToDeclRefExpr(DRE);

      // Don't recur into child closures. They should already have a capture
      // list computed; we just propagate it, filtering out stuff that they
      // capture from us.
      if (auto *SubCE = dyn_cast<AbstractClosureExpr>(E)) {
        for (auto D : SubCE->getCaptureInfo().getCaptures())
          if (D->getDeclContext() != CurExprAsDC)
            captures.insert(D);
        return { false, E };
      }
      return { true, E };
    }

    std::pair<bool, Expr *> walkToDeclRefExpr(DeclRefExpr *DRE) {
      auto *D = DRE->getDecl();

      // Decl references that are within the Capture are local references, ones
      // from parent context are captures.
      if (!CurExprAsDC->isChildContextOf(D->getDeclContext()))
        return { false, DRE };

      // Only capture var decls at global scope.  Other things can be captured
      // if they are local.
      if (!isa<VarDecl>(D) &&
          !DRE->getDecl()->getDeclContext()->isLocalContext())
        return { false, DRE };

      captures.insert(D);
      return { false, DRE };
    }

  };
}

void TypeChecker::computeCaptures(AnyFunctionRef AFR) {
  llvm::SetVector<ValueDecl *> Captures;
  FindCapturedVars finder(Captures, AFR);
  finder.doWalk(AFR.getBody());
  ValueDecl **CaptureCopy =
      Context.AllocateCopy<ValueDecl *>(Captures.begin(), Captures.end());
  AFR.getCaptureInfo().setCaptures(
      llvm::makeArrayRef(CaptureCopy, Captures.size()));
}

