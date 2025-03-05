/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "brw_reg.h"

struct brw_shader;
class brw_builder;

struct brw_vs_thread_payload {
   brw_reg urb_handles;
};

struct brw_tcs_thread_payload {
   brw_reg patch_urb_output;
   brw_reg primitive_id;
   brw_reg icp_handle_start;
};

struct brw_tes_thread_payload {
   brw_reg patch_urb_input;
   brw_reg primitive_id;
   brw_reg coords[3];
   brw_reg urb_output;
};

struct brw_gs_thread_payload {
   brw_reg urb_handles;
   brw_reg primitive_id;
   brw_reg instance_id;
   brw_reg icp_handle_start;
};

struct brw_fs_thread_payload {
   uint8_t subspan_coord_reg[2];
   uint8_t source_depth_reg[2];
   uint8_t source_w_reg[2];
   uint8_t aa_dest_stencil_reg[2];
   uint8_t sample_pos_reg[2];
   uint8_t sample_mask_in_reg[2];
   uint8_t barycentric_coord_reg[INTEL_BARYCENTRIC_MODE_COUNT][2];

   uint8_t depth_w_coef_reg;
   uint8_t pc_bary_coef_reg;
   uint8_t npc_bary_coef_reg;
   uint8_t sample_offsets_reg;
};

struct brw_cs_thread_payload {
   void load_subgroup_id(const brw_builder &bld, brw_reg &dest) const;

   brw_reg local_invocation_id[3];
   brw_reg inline_parameter;
   brw_reg subgroup_id_;
};

struct brw_task_mesh_thread_payload {
   brw_reg inline_parameter;

   brw_reg extended_parameter_0;
   brw_reg local_index;

   brw_reg urb_output;

   /* URB to read Task memory inputs. Only valid for MESH stage. */
   brw_reg task_urb_input;
};

struct brw_bs_thread_payload {
   brw_reg inline_parameter;

   brw_reg global_arg_ptr;
   brw_reg local_arg_ptr;

   void load_shader_type(const brw_builder &bld, brw_reg &dest) const;
};


