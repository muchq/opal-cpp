package io.smithycpp.codegen;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import software.amazon.smithy.model.Model;
import software.amazon.smithy.model.shapes.MemberShape;
import software.amazon.smithy.model.shapes.ShapeId;

/**
 * RecursionIndex decides which generated members get opal::Boxed indirection and which cycles the
 * generator must refuse — decisions previously pinned only by whole-model goldens.
 */
class RecursionIndexTest {

  private static Model model() {
    return Model.assembler()
        .addUnparsedModel(
            "cycles.smithy",
            """
            $version: "2.0"
            namespace test.cycles

            structure SelfLoop {
                next: SelfLoop
                label: String
            }

            structure PingShape {
                pong: PongShape
            }
            structure PongShape {
                ping: PingShape
            }

            structure ListCycle {
                children: ListCycleList
            }
            list ListCycleList { member: ListCycle }

            structure Plain {
                leaf: String
                other: SelfLoop
            }

            union CyclicUnion {
                self: UnionCarrier
                done: String
            }
            structure UnionCarrier {
                back: CyclicUnion
            }

            map CyclicMap {
                key: String
                value: MapCarrier
            }
            structure MapCarrier {
                deeper: CyclicMap
            }
            """)
        .assemble()
        .unwrap();
  }

  private static MemberShape member(Model model, String id) {
    return model.expectShape(ShapeId.from(id), MemberShape.class);
  }

  @Test
  void directSelfRecursionBoxes() {
    Model m = model();
    RecursionIndex index = new RecursionIndex(m);
    assertTrue(index.inCycle(ShapeId.from("test.cycles#SelfLoop")));
    assertTrue(index.isBoxed(member(m, "test.cycles#SelfLoop$next")));
    // The scalar member of a cyclic shape is not itself a cycle edge.
    assertFalse(index.cyclicEdge(member(m, "test.cycles#SelfLoop$label")));
  }

  @Test
  void mutualRecursionBoxesBothEdges() {
    Model m = model();
    RecursionIndex index = new RecursionIndex(m);
    assertTrue(index.isBoxed(member(m, "test.cycles#PingShape$pong")));
    assertTrue(index.isBoxed(member(m, "test.cycles#PongShape$ping")));
  }

  @Test
  void listMediatedCyclesDoNotBox() {
    // std::vector supports incomplete elements behind a forward declaration,
    // so the struct->list edge stays unboxed even though it is on a cycle.
    Model m = model();
    RecursionIndex index = new RecursionIndex(m);
    MemberShape children = member(m, "test.cycles#ListCycle$children");
    assertTrue(index.cyclicEdge(children));
    assertFalse(index.isBoxed(children));
  }

  @Test
  void referencesIntoACycleFromOutsideDoNotBox() {
    Model m = model();
    RecursionIndex index = new RecursionIndex(m);
    assertFalse(index.inCycle(ShapeId.from("test.cycles#Plain")));
    // Plain -> SelfLoop points AT a cycle without being ON it.
    assertFalse(index.isBoxed(member(m, "test.cycles#Plain$other")));
    assertNull(index.unsupportedCycleMember(member(m, "test.cycles#Plain$other")));
  }

  @Test
  void unionAndMapValueCyclesAreNamedUnsupported() {
    Model m = model();
    RecursionIndex index = new RecursionIndex(m);
    String union = index.unsupportedCycleMember(member(m, "test.cycles#CyclicUnion$self"));
    assertTrue(union.contains("recursion through union member"), union);
    assertTrue(union.contains("std::variant needs complete alternatives"), union);
    String map = index.unsupportedCycleMember(member(m, "test.cycles#CyclicMap$value"));
    assertTrue(map.contains("recursion through map value"), map);
    // The structure edges of those same cycles are the supported, boxed ones.
    assertEquals(null, index.unsupportedCycleMember(member(m, "test.cycles#UnionCarrier$back")));
  }
}
