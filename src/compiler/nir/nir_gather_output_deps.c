/*
 * Copyright © 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

/* For each output slot, gather which input components are used to compute it.
 * Component-wise ALU instructions must be scalar.
 */

#include "nir_builder.h"
#include "util/hash_table.h"
#include "util/u_memory.h"

static void
print_output_info(nir_output_deps *deps, FILE *f)
{
   unsigned i;

   BITSET_FOREACH_SET(i, deps->inputs, NUM_TOTAL_VARYING_SLOTS * 8) {
      fprintf(f, " %u.%c%s", i / 8, "xyzw"[(i % 8) / 2], i % 2 ? ".hi" : "");
   }
   fprintf(f, "%s%s%s", deps->uses_output_load ? " (output_load)" : "",
           deps->uses_ssbo_reads ? " (ssbo read)" : "",
           deps->uses_image_reads ? " (image read)" : "");
}

void
nir_print_output_deps(nir_outputs_deps *deps, nir_shader *nir, FILE *f)
{
   for (unsigned i = 0; i < deps->num_locations; i++) {
      fprintf(f, "%s(->%s): %s =",
              _mesa_shader_stage_to_abbrev(nir->info.stage),
              nir->info.next_stage != MESA_SHADER_NONE ?
                 _mesa_shader_stage_to_abbrev(nir->info.next_stage) :
                 "NONE",
              gl_varying_slot_name_for_stage(deps->locations[i],
                                             nir->info.stage));

      print_output_info(&deps->output[i], f);
      fprintf(f, "\n");
   }
}

static void
accum_deps(nir_output_deps *dst, nir_output_deps *src)
{
   BITSET_OR(dst->inputs, dst->inputs, src->inputs);
   dst->uses_output_load |= src->uses_output_load;
   dst->uses_ssbo_reads |= src->uses_ssbo_reads;
   dst->uses_image_reads |= src->uses_image_reads;
}

static bool
accum_src_deps(nir_src *src, void *opaque)
{
   nir_output_deps *instr_deps = (nir_output_deps *)opaque;
   nir_instr *src_instr = src->ssa->parent_instr;

   if (src_instr->type == nir_instr_type_load_const ||
       src_instr->type == nir_instr_type_undef)
      return true;

   nir_instr *dst_instr = nir_src_parent_instr(src);
   accum_deps(&instr_deps[dst_instr->index], &instr_deps[src_instr->index]);
   return true;
}

static bool
gather_has_loop_phi(nir_src *src, void *opaque)
{
   nir_instr *phi = nir_src_parent_instr(src);
   nir_instr *src_instr = src->ssa->parent_instr;
   bool *has_loop_phi = (bool*)opaque;

   *has_loop_phi |= phi->index < src_instr->index;
   return !*has_loop_phi;
}

static unsigned
get_slot_index(nir_intrinsic_instr *intr, unsigned slot_offset)
{
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   return (sem.location + slot_offset) * 8 + nir_intrinsic_component(intr) * 2 +
          sem.high_16bits;
}

/* For each output slot, gather which input components are used to compute it.
 * IO intrinsics must be scalar. Component-wise ALU instructions should be
 * scalar, but if they are not, the result will have false positives.
 */
void
nir_gather_output_dependencies(nir_shader *nir, nir_outputs_deps *deps)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   unsigned num_instr = nir_index_instrs(impl);
   nir_output_deps *instr_deps = calloc(num_instr, sizeof(nir_output_deps));
   bool has_loop_phi = false;
   bool second_pass = false;

   memset(deps->output, 0, sizeof(deps->output));

   /* Gather dependencies of every instruction.
    * Dependencies of each instruction are OR'd dependencies of its sources.
    */
again:
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         /* Dependencies of each instruction are OR'd dependencies of its
          * sources.
          */
         nir_foreach_src(instr, accum_src_deps, instr_deps);

         if (second_pass && instr->type != nir_instr_type_intrinsic)
            continue;

         nir_output_deps *cur_deps = &instr_deps[instr->index];

         /* Gather the current instruction. */
         switch (instr->type) {
         case nir_instr_type_tex:
            if (!nir_tex_instr_is_query(nir_instr_as_tex(instr)))
               cur_deps->uses_image_reads = true;
            break;

         case nir_instr_type_phi:
            if (!has_loop_phi)
               nir_foreach_src(instr, gather_has_loop_phi, &has_loop_phi);
            break;

         case nir_instr_type_intrinsic: {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            switch (intr->intrinsic) {
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_input_vertex:
            case nir_intrinsic_load_per_vertex_input:
            case nir_intrinsic_load_per_primitive_input:
            case nir_intrinsic_load_interpolated_input: {
               if (second_pass)
                  continue;

               nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
               assert(intr->def.num_components == 1);
               assert(sem.num_slots >= 1);

               for (unsigned i = 0; i < sem.num_slots; i++)
                  BITSET_SET(cur_deps->inputs, get_slot_index(intr, i));
               break;
            }
            case nir_intrinsic_load_output:
            case nir_intrinsic_load_per_vertex_output:
               cur_deps->uses_output_load = true;
               break;

            default: {
               if (second_pass)
                  continue;

               const char *name = nir_intrinsic_infos[intr->intrinsic].name;

               if (strstr(name, "load_ssbo") || strstr(name, "ssbo_atomic"))
                  cur_deps->uses_ssbo_reads = true;

               if (strstr(name, "image") &&
                   (strstr(name, "load") || strstr(name, "atomic")))
                  cur_deps->uses_image_reads = true;
               break;
            }

            case nir_intrinsic_store_output:
            case nir_intrinsic_store_per_vertex_output:
            case nir_intrinsic_store_per_primitive_output:
            case nir_intrinsic_store_per_view_output: {
               /* The write mask must be contigous starting from x. */
               ASSERTED unsigned writemask = nir_intrinsic_write_mask(intr);
               assert(writemask == BITFIELD_MASK(util_bitcount(writemask)));

               /* Check whether we were asked to gather this output. */
               nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
               assert(sem.num_slots >= 1);

               for (unsigned i = 0; i < deps->num_locations; i++) {
                  if (deps->locations[i] >= (int)sem.location &&
                      deps->locations[i] < (int)(sem.location + sem.num_slots))
                     accum_deps(&deps->output[i], cur_deps);
               }
               break;
            }
            }
            break;
         }

         default:
            break;
         }
      }
   }

   /* If there is a loop, do it again and only OR sources together. */
   if (has_loop_phi && !second_pass) {
      second_pass = true;
      goto again;
   }
}

/* Gather 3 disjoint sets:
 * - the set of input components only used to compute outputs for the clipper
 *   (those that are only used to compute the position and clip outputs)
 * - the set of input components only used to compute all other outputs
 * - the set of input components that are used to compute BOTH outputs for
 *   the clipper and all other outputs
 *
 * Patch outputs are not gathered because shaders feeding the clipper don't
 * have patch outputs.
 */
void
nir_gather_output_clipper_var_groups(nir_shader *nir,
                                     nir_output_clipper_var_groups *groups)
{
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

   /* Use calloc because these are large structures. */
   nir_outputs_deps *pos_deps = calloc(1, sizeof(nir_outputs_deps));
   nir_outputs_deps *var_deps = calloc(1, sizeof(nir_outputs_deps));

   uint64_t clipper_outputs = VARYING_BIT_POS |
                              VARYING_BIT_CLIP_VERTEX |
                              VARYING_BIT_CLIP_DIST0 |
                              VARYING_BIT_CLIP_DIST1;

   /* Gather input components used to compute outputs for the clipper. */
   u_foreach_bit64(i, nir->info.outputs_written & clipper_outputs) {
      pos_deps->locations[pos_deps->num_locations++] = i;
   }

   if (pos_deps->num_locations)
      nir_gather_output_dependencies(nir, pos_deps);

   /* Gather input components used to compute all other outputs. */
   u_foreach_bit64(i, nir->info.outputs_written & ~clipper_outputs) {
      var_deps->locations[var_deps->num_locations++] = i;
   }
   u_foreach_bit(i, nir->info.outputs_written_16bit) {
      var_deps->locations[var_deps->num_locations++] =
         VARYING_SLOT_VAR0_16BIT + i;
   }

   if (var_deps->num_locations)
      nir_gather_output_dependencies(nir, var_deps);

   /* OR-reduce the per-output sets. */
   memset(groups, 0, sizeof(*groups));

   for (unsigned i = 0; i < pos_deps->num_locations; i++) {
      assert(!pos_deps->output[i].uses_output_load);
      BITSET_OR(groups->pos_only, groups->pos_only,
                pos_deps->output[i].inputs);
   }

   for (unsigned i = 0; i < var_deps->num_locations; i++) {
      assert(!var_deps->output[i].uses_output_load);
      BITSET_OR(groups->var_only, groups->var_only,
                var_deps->output[i].inputs);
   }

   /* Compute the intersection of the above and make them disjoint. */
   BITSET_AND(groups->both, groups->pos_only, groups->var_only);
   BITSET_ANDNOT(groups->pos_only, groups->pos_only, groups->both);
   BITSET_ANDNOT(groups->var_only, groups->var_only, groups->both);

   free(pos_deps);
   free(var_deps);
}
