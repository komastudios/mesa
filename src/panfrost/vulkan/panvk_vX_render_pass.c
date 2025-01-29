/*
 * Copyright © 2025 Arm Ltd.
 * Copyright © 2025 Collabora Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#include "vk_common_entrypoints.h"
#include "vk_render_pass.h"

#include "panvk_cmd_buffer.h"

static bool
fits_in_tile_buf(const struct vk_render_pass *pass,
                 const struct vk_subpass_merging_ctx *ctx)
{
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(pass->base.device->physical);
   uint32_t tile_buf_budget = panfrost_query_optimal_tib_size(phys_dev->model);
   uint32_t total_bpp = 0;
   u_foreach_bit(i, ctx->attachments.used_color_mask) {
      const struct vk_subpass_merging_attachment_ref ref =
         ctx->attachments.colors[i];
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(pass, ref.subpass);
      uint32_t att_idx = subpass->color_attachments[ref.index].attachment;
      const struct vk_render_pass_attachment *att = &pass->attachments[att_idx];
      enum pipe_format pfmt = vk_format_to_pipe_format(att->format);
      const struct pan_blendable_format *bf =
         GENX(panfrost_blendable_format_from_pipe_format)(pfmt);
      uint32_t rt_bpp =
         bf->internal ? 4
                      : util_next_power_of_two(util_format_get_blocksize(pfmt));
      rt_bpp *= att->samples;
      total_bpp += rt_bpp;
   }

   /* Let's aim for a 16x16 effective tile size. */
   return total_bpp * 16 * 16 <= tile_buf_budget;
}

static bool
first_subpass_outputs_reused(const struct vk_subpass_merging_ctx *ctx)
{
   u_foreach_bit(c, ctx->attachments.used_color_mask) {
      if (ctx->attachments.colors[c].subpass == ctx->first_subpass &&
          ctx->attachments.colors[c].access_count > 1)
         return true;
   }

   if (ctx->attachments.depth.subpass == ctx->first_subpass &&
       ctx->attachments.depth.access_count > 1)
      return true;

   if (ctx->attachments.stencil.subpass == ctx->first_subpass &&
       ctx->attachments.stencil.access_count > 1)
      return true;

   return false;
}

static bool
is_mergeable(const struct vk_render_pass *pass,
             const struct vk_subpass_merging_ctx *ctx,
             uint32_t *first_subpass, uint32_t *last_subpass)
{
   /* Single subpass, nothing to merge. */
   if (ctx->first_subpass == ctx->last_subpass) {
      *first_subpass = ctx->first_subpass + 1;
      *last_subpass = pass->subpass_count - 1;
      return false;
   }

   /* If the render targets of the first subpass are not reused as input
    * attachments or RTs, there's no point merging this RT with the
    * following ones. Let's just tweak the a single */
   if (!first_subpass_outputs_reused(ctx)) {
      *first_subpass = ctx->first_subpass + 1;
      *last_subpass = pass->subpass_count - 1;
      return false;
   }

   /* If we don't have enough tile buffer space, shorten the considered
    * subpass range by evicting the last subpass. */
   if (!fits_in_tile_buf(pass, ctx)) {
      *last_subpass = ctx->last_subpass - 1;
      return false;
   }

   *first_subpass = ctx->last_subpass + 1;
   *last_subpass = pass->subpass_count - 1;
   return true;
}

static VkResult
merge_subpasses(struct vk_render_pass *pass, const VkAllocationCallbacks *alloc)
{
   uint32_t first_sp = 0, last_sp = pass->subpass_count - 1;
   struct vk_subpass_merging_ctx ctx;

   do {
      vk_render_pass_next_mergeable_range(pass, first_sp, last_sp, &ctx);

      if (is_mergeable(pass, &ctx, &first_sp, &last_sp)) {
         VkResult result = vk_render_pass_merge_subpasses(pass, alloc, &ctx);
         if (result != VK_SUCCESS)
            return result;
      }
   } while(first_sp < pass->subpass_count);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(CreateRenderPass2)(VkDevice device,
                                  const VkRenderPassCreateInfo2 *pCreateInfo,
                                  const VkAllocationCallbacks *pAllocator,
                                  VkRenderPass *pRenderPass)
{
   VkResult result =
      vk_common_CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

   if (result != VK_SUCCESS)
      return result;

   VK_FROM_HANDLE(vk_render_pass, pass, *pRenderPass);

   result = merge_subpasses(pass, pAllocator);
   if (result != VK_SUCCESS)
      vk_common_DestroyRenderPass(device, *pRenderPass, pAllocator);

   return result;
}
