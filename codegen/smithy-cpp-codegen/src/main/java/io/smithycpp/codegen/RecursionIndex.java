package io.smithycpp.codegen;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.Shape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * Strongly connected components over the model's aggregate shapes (structures, unions, lists,
 * maps), driving boxed-recursion support: a structure member whose target is a structure on the
 * same cycle is represented as {@code opal::Boxed<T>} (heap indirection with value semantics);
 * cycles through lists need only a forward declaration ({@code std::vector} permits incomplete
 * element types). Cycles through union members or map values are not supported yet — {@link
 * #unsupportedCycleMember(MemberShape)} names them so generation fails with a clear message instead
 * of emitting non-compiling code.
 */
final class RecursionIndex {

  private final Model model;

  /** SCC id per aggregate shape. */
  private final Map<ShapeId, Integer> component = new HashMap<>();

  /** Shapes on a cycle: SCC size > 1, or a self-loop. */
  private final Set<ShapeId> cyclic = new HashSet<>();

  RecursionIndex(Model model) {
    this.model = model;
    computeComponents();
  }

  /** Whether the shape participates in any recursion cycle. */
  boolean inCycle(ShapeId id) {
    return cyclic.contains(id);
  }

  private boolean sameCycle(ShapeId a, ShapeId b) {
    return cyclic.contains(a)
        && cyclic.contains(b)
        && Objects.equals(component.get(a), component.get(b));
  }

  /** Whether the member is on a cycle with its container (its edge can close the loop). */
  boolean cyclicEdge(MemberShape member) {
    return sameCycle(member.getContainer(), member.getTarget());
  }

  /** Structure member targeting a structure on the same cycle: emitted as opal::Boxed. */
  boolean isBoxed(MemberShape member) {
    if (!cyclicEdge(member)) {
      return false;
    }
    Shape container = model.expectShape(member.getContainer());
    Shape target = model.expectShape(member.getTarget());
    return container.isStructureShape() && target.isStructureShape();
  }

  /**
   * A description of why this member's cycle is unsupported (union members and map values on a
   * cycle), or null when the member is fine.
   */
  String unsupportedCycleMember(MemberShape member) {
    if (!cyclicEdge(member)) {
      return null;
    }
    Shape container = model.expectShape(member.getContainer());
    if (container.isUnionShape()) {
      return "recursion through union member "
          + member.getId()
          + " is not supported yet (std::variant needs complete alternatives)";
    }
    if (container.isMapShape() && member.getMemberName().equals("value")) {
      return "recursion through map value "
          + member.getId()
          + " is not supported yet (std::map needs a complete mapped type)";
    }
    return null;
  }

  private static boolean aggregate(Shape shape) {
    return shape.isStructureShape()
        || shape.isUnionShape()
        || shape.isListShape()
        || shape.isMapShape();
  }

  private List<ShapeId> neighbors(Shape shape) {
    List<ShapeId> out = new ArrayList<>();
    for (MemberShape member : shape.members()) {
      Shape target = model.expectShape(member.getTarget());
      if (aggregate(target)) {
        out.add(target.getId());
      }
    }
    return out;
  }

  /** Iterative Tarjan SCC (models can nest deeply enough to matter). */
  private void computeComponents() {
    Map<ShapeId, Integer> index = new HashMap<>();
    Map<ShapeId, Integer> lowLink = new HashMap<>();
    Set<ShapeId> onStack = new HashSet<>();
    Deque<ShapeId> stack = new ArrayDeque<>();
    int[] counter = {0};
    int[] componentCounter = {0};

    for (Shape start : model.toSet()) {
      if (!aggregate(start) || index.containsKey(start.getId())) {
        continue;
      }
      // Explicit DFS frames: (shape id, next neighbor position).
      Deque<ShapeId> path = new ArrayDeque<>();
      Deque<Integer> position = new ArrayDeque<>();
      path.push(start.getId());
      position.push(0);
      index.put(start.getId(), counter[0]);
      lowLink.put(start.getId(), counter[0]);
      counter[0]++;
      stack.push(start.getId());
      onStack.add(start.getId());
      while (!path.isEmpty()) {
        ShapeId current = path.peek();
        List<ShapeId> next = neighbors(model.expectShape(current));
        int at = position.pop();
        if (at < next.size()) {
          position.push(at + 1);
          ShapeId neighbor = next.get(at);
          if (!index.containsKey(neighbor)) {
            path.push(neighbor);
            position.push(0);
            index.put(neighbor, counter[0]);
            lowLink.put(neighbor, counter[0]);
            counter[0]++;
            stack.push(neighbor);
            onStack.add(neighbor);
          } else if (onStack.contains(neighbor)) {
            lowLink.put(current, Math.min(lowLink.get(current), index.get(neighbor)));
          }
          continue;
        }
        path.pop();
        if (!path.isEmpty()) {
          ShapeId parent = path.peek();
          lowLink.put(parent, Math.min(lowLink.get(parent), lowLink.get(current)));
        }
        if (lowLink.get(current).equals(index.get(current))) {
          List<ShapeId> members = new ArrayList<>();
          ShapeId popped;
          do {
            popped = stack.pop();
            onStack.remove(popped);
            members.add(popped);
            component.put(popped, componentCounter[0]);
          } while (!popped.equals(current));
          componentCounter[0]++;
          if (members.size() > 1) {
            cyclic.addAll(members);
          } else {
            // Self-loop: a shape with a member targeting itself.
            ShapeId only = members.get(0);
            for (ShapeId neighbor : neighbors(model.expectShape(only))) {
              if (neighbor.equals(only)) {
                cyclic.add(only);
                break;
              }
            }
          }
        }
      }
    }
  }
}
