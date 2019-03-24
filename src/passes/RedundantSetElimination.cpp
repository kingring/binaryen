/*
 * Copyright 2017 WebAssembly Community Group participants
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
// Eliminate redundant local.sets: if a local already has a particular
// value, we don't need to set it again. A common case here is loops
// that start at zero, since the default value is initialized to
// zero anyhow.
//
// A risk here is that we extend live ranges, e.g. we may use the default
// value at the very end of a function, keeping that local alive throughout.
// For that reason it is probably better to run this near the end of
// optimization, and especially after coalesce-locals. A final vaccum
// should be done after it, as this pass can leave around drop()s of
// values no longer necessary.
//

#include <wasm.h>
#include <pass.h>
#include <wasm-builder.h>
#include <ir/find_all.h>
#include <ir/local-graph.h>
#include <ir/literal-utils.h>
#include <ir/utils.h>

namespace wasm {

#if 0
// Finds which sets are equivalent, that is, must contain the same value.
class Equivalences {
public:
  Equivalences(Function* func) : func(func), graph(func) {
    compute();
  }

  bool areEquivalent(SetLocal* a, SetLocal* b) {
    return getKnownClass(a) == getKnownClass(b);
  }

  // Return the class. 0 is the "null class" - we haven't calculated it yet.
  Index getClass(SetLocal* set) {
    auto iter = equivalenceClasses.find(set);
    if (iter == equivalenceClasses.end()) {
      return 0;
    }
    auto ret = iter->second;
    assert(ret != 0);
    return ret;
  }

  Index getKnownClass(SetLocal* set) {
    auto ret = getClass(set);
    assert(ret != 0);
    return ret;
  }

  bool known(SetLocal* set) {
    return getClass(set) != 0;
  }

private:
  Function* func;
  LocalGraph graph;

  // There is a unique id for each class, which this maps sets to.
  std::unordered_map<SetLocal*, Index> equivalenceClasses;

  void compute() {
    FindAll<SetLocal> allSets(func->body);
    // Set up the graph of direct connections. We'll use this to calculate the final
    // equivalence classes (since being equivalent is a symmetric, transitivie, and
    // reflexive operation).
    struct Node {
      SetLocal* set;

      std::vector<Node*> directs; // direct equivalences, resulting from copying a value
      std::vector<Node*> mergesIn, mergesOut;

      void addDirect(Node* other) {
        directs.push_back(other);
        other->directs.push_back(this);
      }
    };
    std::vector<std::unique_ptr<Node>> nodes;
    std::map<SetLocal*, Node*> setNodes;
    // Add sets in the function body.
    for (auto* set : allSets) {
      auto node = make_unique<Node>();
      node->set = set;
      setNodes.emplace(set, node.get());
      nodes.push_back(std::move(node));
    }
    // Note sets of a constant, and connect them all.
    std::map<Literal, Node*> literalNodes;
    // Add connections.
    for (auto& node : nodes) {
      auto* set = node->set;
      auto* value = set->value;
      // Look through trivial fallthrough-ing (but stop if the value were used - TODO?)
      value = Properties::getUnusedFallthrough(value);
      if (auto* tee = value->dynCast<SetLocal>()) {
        node->addDirect(setNodes[tee]);
      } else if (auto* get = value->dynCast<GetLocal>()) {
        auto& sets = getSets.getSetsFor(get);
        if (sets.size() == 1) {
          node->addDirect(setNodes[*sets.begin()]);
        } else if (sets.size() > 1) {
          for (auto* otherSet : sets) {
            auto& otherNode = setNodes[otherSet];
            node->mergesIn.push_back(otherNode);
            otherNode->mergesOut.push_back(node.get());
          }
        }
      } else if (auto* c = value->dynCast<Const>()) {
        auto literal = c->value;
        auto iter = literalNodes.find(literal);
        if (iter != literalNodes.end()) {
          node->addDirect(iter->second);
        } else {
          literalNodes[literal] = node.get();
        }
      }
    }
    // Calculating the final classes is mostly a simple floodfill operation,
    // however, merges are more interesting: we can only see that a merge
    // set is equivalent to another if all the things it merges are equivalent.
    Index currClass = 0;
    for (auto& start : nodes) {
      if (known(start->set)) continue;
      currClass++;
      // Floodfill the current node.
      WorkList<Node*> work;
      work.push(start.get());
      while (!work.empty()) {
        auto* curr = work.pop();
        auto* set = curr->set;
        // At this point the class may be unknown, or it may be another class - consider
        // the case that A and B are linked, and merge into C, and we start from C. Then C
        // by itself can do nothing yet, until we first see the other two are identical,
        // and get prompted to look again at C. In that case, we will trample the old
        // class. In other words, we should only stop here if we see the class we are
        // currently flooding (as we can do nothing more for it).
        if (getClass(set) == currClass) continue;
        equivalenceClasses[set] = currClass;
        for (auto* direct : curr->directs) {
          work.push(direct);
        }
        // Check outgoing merges - we may have enabled a node to be marked as
        // being in this equivalence class.
        for (auto* mergeOut : curr->mergesOut) {
          if (getClass(mergeOut->set) == currClass) continue;
          assert(!mergeOut->mergesIn.empty());
          bool ok = true;
          for (auto* mergeIn : mergeOut->mergesIn) {
            if (getClass(mergeIn->set) != currClass) {
              ok = false;
              break;
            }
          }
          if (ok) {
            work.push(mergeOut);
          }
        }
      }
    }

#if CFG_DEBUG
    for (auto& node : nodes) {
      auto* set = node->set;
      std::cout << "set " << set << " has index " << set->index << " and class " << getClass(set) << '\n';
    }
#endif
  }
};

#endif

namespace {

// Instrumentation helpers.

static Expression*& getInstrumentedValue(SetLocal* set) {
  return set->value->cast<Block>()->list[0]->cast<Drop>()->value;
}

static GetLocal* getInstrumentedGet(SetLocal* set) {
  return set->value->cast<Block>()->list[1]->cast<GetLocal>();
}

// Marks a set as unneeded, and so we can remove it during uninstrumentation.
// We remove the get from it, which is no longer needed anyhow at this point.
static void markSetAsUnneeded(SetLocal* set) {
  set->value->cast<Block>()->list.pop_back();
}

static void isSetUnneeded(SetLocal* set) {
  return set->value->cast<Block>()->list.size() == 1;
}

// Main class.

struct RedundantSetElimination : public WalkerPass<PostWalker<RedundantSetElimination>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new RedundantSetElimination(); }

  // main entry point

  void doWalkFunction(Function* func) {
    // Instrument the function so we can tell what value is present at a local
    // index right before each set.
    instrument(func);
    // Compute the getSets across the instrumented function.
    LocalGraph graph_(func);
    graph = graph_;
    // Remove redundant sets.
    WalkerPass<PostWalker<RedundantSetElimination>>::doWalkFunction(func);
    // Clean up.
    unInstrument(func);
  }

  LocalGraph* graph;

  void visitSetLocal(SetLocal* curr) {
    if (curr->type == unreachable) return;
    auto* getBeforeSet = getInstrumentedGet(curr);
    auto& sets = graph->getSetses[getBeforeSet];
    if (sets.size() == 1) {
      auto* parent = *sets.begin();
      auto* currValue = getInstrumentedValue(curr);
      if (!parent) {
        if (LiteralUtils::isEqualTo(currValue, Literal::makeZero(curr->value->type))) {
          markSetAsUnneeded(curr);
        }
      } else {
        auto* parentValue = getInstrumentedValue(parent);
        if (LiteralUtils::isEqualTo(parentValue, currValue)) {
          markSetAsUnneeded(curr);
        }
      }        
    }
  }

private:
  MixedArena tempAllocations;

  void instrument(Function* func) {
    // We replace
    //  (local.set $x
    //    (value)
    //  )
    // with
    //  (local.set $x
    //    (block
    //      (drop (value))
    //      (local.get $x)
    //    )
    //  )
    // Note that this changes the logic, but all we care about is being able
    // to find the sets for that get that happens right before the set.
    struct Instrumenter : public PostWalker<Instrumenter> {
      MixedArena& tempAllocations;

      Instrumenter(Function* func, MixedArena& tempAllocations) : tempAllocations(tempAllocations) {
        walk(func->body);
      }

      void visitSetLocal(SetLocal* curr) {
        if (curr->type == unreachable) return;
        Builder builder(tempAllocations);
        set->value = builder.makeSequence(
          builder.makeDrop(curr->value),
          builder.makeGetLocal(curr->index, curr->value->type)
        );
      }
    } instrumenter(func, tempAllocations);
  }

  void unInstrument(Function* func) {
    struct UnInstrumenter : public PostWalker<UnInstrumenter> {
      Instrumenter(Function* func) {
        walk(func->body);
      }

      void visitSetLocal(SetLocal* curr) {
        if (curr->type == unreachable) return;
        auto* value = getInstrumentedValue(curr);
        if (!isSetUnneeded(set)) {
          curr->value = value;
        } else {
          ExpressionManipulator::convert<SetLocal, Drop>(curr);
          curr->value = value;
        }
      }
    } unInstrumenter(func);
  }
};

} // anonymous namespace

Pass *createRedundantSetEliminationPass() {
  return new RedundantSetElimination();
}

} // namespace wasm
