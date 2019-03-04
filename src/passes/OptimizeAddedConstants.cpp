/*
 * Copyright 2019 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Optimize added constants into load/store offsets. This requires the assumption
// that low memory is unused, so that we can replace an add (which might wrap)
// with a load/store offset (which does not).
//
// The propagate option also propagates offsets across set/get local pairs.
//
// Optimizing constants into load/store offsets is almost always
// beneficial for speed, as VMs can optimize these operations better.
// If a LocalGraph is provided, this can also propagate values along get/set
// pairs. In such a case, we may increase code size slightly or reduce
// compressibility (e.g., replace (load (get $x)) with (load offset=Z (get $y)),
// where Z is big enough to not fit in a single byte), but this is good for
// speed, and may lead to code size reductions elsewhere by using fewer locals.
//

#include <wasm.h>
#include <pass.h>
#include <wasm-builder.h>
#include <ir/local-graph.h>

namespace wasm {

template<typename T>
class MemoryAccessOptimizer {
public:
  MemoryAccessOptimizer(T* curr, Module* module, LocalGraph* localGraph) : curr(curr), module(module), localGraph(localGraph) {
    // The pointer itself may be a constant, if e.g. it was precomputed or
    // a get that we propagated.
    if (curr->ptr->template is<Const>()) {
      optimizeConstantPointer();
      return;
    }
    if (auto* add = curr->ptr->template dynCast<Binary>()) {
      if (add->op == AddInt32) {
        // Look for a constant on both sides.
        if (tryToOptimizeConstant(add->right, add->left) ||
            tryToOptimizeConstant(add->left, add->right)) {
          return;
        }
      }
    }
    if (localGraph) {
      // A final important case is a propagated add:
      //
      //  x = y + 10
      //  ..
      //  load(x)
      // =>
      //  x = y + 10
      //  ..
      //  load(y, offset=10)
      //
      // This is only valid if y does not change in the middle!
      if (auto* get = curr->ptr->template dynCast<GetLocal>()) {
        auto& sets = localGraph->getSetses[get];
        if (sets.size() == 1) {
          auto* set = *sets.begin();
          // May be a zero-init (in which case, we can ignore it).
          if (set) {
            auto* value = set->value;
            if (auto* add = value->template dynCast<Binary>()) {
              if (add->op == AddInt32) {
                // We can optimize on either side, but only if both we find
                // a constant *and* the other side cannot change in the middle.
                // TODO If it could change, we may add a new local to capture the
                //      old value.
                if (tryToOptimizePropagatedAdd(add->right, add->left, get) ||
                   tryToOptimizePropagatedAdd(add->left, add->right, get)) {
                  return;
                }
              }
            }
          }
        }
      }
    }
  }

private:
  T* curr;
  Module* module;
  LocalGraph* localGraph;

  void optimizeConstantPointer() {
    // The constant and an offset are interchangeable:
    //   (load (const X))  <=>  (load offset=X (const 0))
    // It may not matter if we do this or not - it's the same size,
    // and in both cases the compiler can see it's a constant location.
    // For code clarity and compressibility, we prefer to put the
    // entire address in the constant.
    if (curr->offset) {
      // Note that the offset may already be larger than low memory - the
      // code may know that is valid, even if we can't. Only handle the
      // obviously valid case where an overflow can't occur.
      auto* c = curr->ptr->template cast<Const>();
      uint32_t base = c->value.geti32();
      uint32_t offset = curr->offset;
      if (uint64_t(base) + uint64_t(offset) < (uint64_t(1) << 32)) {
        c->value = c->value.add(Literal(uint32_t(curr->offset)));
        curr->offset = 0;
      }
    }
  }

  struct Result {
    bool succeeded;
    Address total;
    Result() : succeeded(false) {}
    Result(Address total) : succeeded(true), total(total) {}
  };

  // See if we can optimize an offset from an expression. If we report
  // success, the returned offset can be added as a replacement for the
  // expression here.
  bool tryToOptimizeConstant(Expression* oneSide, Expression* otherSide) {
    if (auto* c = oneSide->template dynCast<Const>()) {
      auto result = canOptimizeConstant(c->value);
      if (result.succeeded) {
        curr->offset = result.total;
        curr->ptr = otherSide;
        if (curr->ptr->template is<Const>()) {
          optimizeConstantPointer();
        }
        return true;
      }
    }
    return false;
  }

  bool tryToOptimizePropagatedAdd(Expression* oneSide, Expression* otherSide, GetLocal* ptr) {
    if (auto* c = oneSide->template dynCast<Const>()) {
      auto result = canOptimizeConstant(c->value);
      if (result.succeeded) {
        // Looks good, but we need to make sure the other side cannot change:
        //
        //  x = y + 10
        //  y = y + 1
        //  load(x)
        //
        // This example should not be optimized into
        //
        //  load(x, offset=10)
        //
        // If the other side is a get, we may be able to prove that we can just use that same
        // local, if both it and the pointer are in SSA form. In that case,
        //
        //  y = .. // single assignment that dominates all uses
        //  x = y + 10 // single assignment that dominates all uses
        //  [..]
        //  load(x) => load(y, offset=10)
        //
        // This is valid since dominance is transitive, so y's definition dominates the load,
        // and it is ok to replace x with y + 10 there.
        // TODO otherwise, create a new local
        if (auto* get = otherSide->template dynCast<GetLocal>()) {
          if (localGraph->isSSA(get->index) && localGraph->isSSA(ptr->index)) {
            curr->offset = result.total;
            curr->ptr = Builder(*module).makeGetLocal(get->index, get->type);
            return true;
          }
        }
      }
    }
    return false;
  }

  // Sees if we can optimize a particular constant.
  Result canOptimizeConstant(Literal literal) {
    auto value = literal.geti32();
    // Avoid uninteresting corner cases with peculiar offsets.
    if (value >= 0 && value < PassOptions::LowMemoryBound) {
      // The total offset must not allow reaching reasonable memory
      // by overflowing.
      auto total = curr->offset + value;
      if (total < PassOptions::LowMemoryBound) {
        return Result(total);
      }
    }
    return Result();
  }
};

struct OptimizeAddedConstants : public WalkerPass<PostWalker<OptimizeAddedConstants, UnifiedExpressionVisitor<OptimizeAddedConstants>>> {
  bool isFunctionParallel() override { return true; }

  bool propagate;

  OptimizeAddedConstants(bool propagate) : propagate(propagate) {}

  Pass* create() override { return new OptimizeAddedConstants(propagate); }

  std::unique_ptr<LocalGraph> localGraph;

  void visitLoad(Load* curr) {
    MemoryAccessOptimizer<Load>(curr, getModule(), localGraph.get());
  }

  void visitStore(Store* curr) {
    MemoryAccessOptimizer<Store>(curr, getModule(), localGraph.get());
  }

  void doWalkFunction(Function* func) {
    // This pass is only valid under the assumption of unused low memory.
    assert(getPassOptions().lowMemoryUnused);
    if (propagate) {
      localGraph = make_unique<LocalGraph>(func);
      localGraph->computeSSAIndexes();
    }
    super::doWalkFunction(func);
  }
};

Pass *createOptimizeAddedConstantsPass() {
  return new OptimizeAddedConstants(false);
}

Pass *createOptimizeAddedConstantsPropagatePass() {
  return new OptimizeAddedConstants(true);
}

} // namespace wasm
