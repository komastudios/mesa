/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"

static bool
pass(nir_builder *b, nir_intrinsic_instr *intrin, void *data)
{
   if (intrin->intrinsic != nir_intrinsic_load_view_index)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);
   nir_def_replace(&intrin->def, nir_imm_zero(b, 1, 32));
   return true;
}

bool
radv_nir_lower_intrinsics_early(nir_shader *nir, bool lower_view_index_to_zero)
{
   bool progress = false;

   NIR_PASS(progress, nir, nir_lower_sparse_resident_query,
            NIR_SPARSE_BIT_ALL | NIR_SPARSE_BIT_INVERTED);

   if (lower_view_index_to_zero) {
      NIR_PASS(progress, nir, nir_shader_intrinsics_pass, pass,
               nir_metadata_control_flow, NULL);
   }

   return progress;
}
