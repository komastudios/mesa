/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#ifndef PANVK_BLEND_H
#define PANVK_BLEND_H

#include <stdbool.h>

#include "util/hash_table.h"
#include "util/simple_mtx.h"

#include "pan_blend.h"

#include "panvk_macros.h"
#include "panvk_mempool.h"

#include "vk_graphics_state.h"

struct panvk_cmd_buffer;

#ifdef PAN_ARCH

struct panvk_blend_info {
   bool any_dest_read;
   bool needs_shader;
   bool shader_loads_blend_const;
};

static inline uint32_t
panvk_blend_desc_count(const struct vk_color_blend_state *cb,
                       const struct vk_color_attachment_location_state *cal)
{
   for (uint32_t i = cb->attachment_count; i > 0; i--) {
      if (cal->color_map[i - 1] != MESA_VK_ATTACHMENT_UNUSED)
         return i;
   }

   return 1;
}

VkResult panvk_per_arch(blend_emit_descs)(struct panvk_cmd_buffer *cmdbuf,
                                          struct mali_blend_packed *bds);

#endif

#endif
