//===----------------------------------------------------------------------===//
//
//                        JFS - The JIT Fuzzing Solver
//
// Copyright 2017 Daniel Liew
//
// This file is distributed under the MIT license.
// See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
#ifndef JFS_CORE_Z3_AST_VISITOR_H
#define JFS_CORE_Z3_AST_VISITOR_H
#include "jfs/Core/Z3Node.h"

namespace jfs {
namespace core {

// FIXME: This design only works for
// read only traversal. It needs rethinking
// for Z3AST modification and traversal order
class Z3ASTVisitor {
public:
  Z3ASTVisitor();
  virtual ~Z3ASTVisitor();
  void visit(Z3ASTHandle e);

protected:
  // TODO: Add more methods for different Z3 application kinds

  // Constants
  virtual void visitBoolConstant(Z3AppHandle e) = 0;
  virtual void visitBitVector(Z3AppHandle e) = 0;
};
}
}
#endif