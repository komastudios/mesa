/*
 * Copyright © 2020 Valve Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "vk_render_pass.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_format.h"
#include "vk_framebuffer.h"
#include "vk_image.h"
#include "vk_physical_device.h"
#include "vk_util.h"

#include "util/log.h"

static void
translate_references(VkAttachmentReference2 **reference_ptr,
                     uint32_t reference_count,
                     const VkAttachmentReference *reference,
                     const VkRenderPassCreateInfo *pass_info,
                     bool is_input_attachment)
{
   VkAttachmentReference2 *reference2 = *reference_ptr;
   *reference_ptr += reference_count;
   for (uint32_t i = 0; i < reference_count; i++) {
      reference2[i] = (VkAttachmentReference2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
         .pNext = NULL,
         .attachment = reference[i].attachment,
         .layout = reference[i].layout,
      };

      if (is_input_attachment &&
          reference2[i].attachment != VK_ATTACHMENT_UNUSED) {
         assert(reference2[i].attachment < pass_info->attachmentCount);
         const VkAttachmentDescription *att =
            &pass_info->pAttachments[reference2[i].attachment];
         reference2[i].aspectMask = vk_format_aspects(att->format);
      }
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateRenderPass(VkDevice _device,
                           const VkRenderPassCreateInfo *pCreateInfo,
                           const VkAllocationCallbacks *pAllocator,
                           VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   uint32_t reference_count = 0;
   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      reference_count += pCreateInfo->pSubpasses[i].inputAttachmentCount;
      reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments)
         reference_count += pCreateInfo->pSubpasses[i].colorAttachmentCount;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment)
         reference_count += 1;
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, VkRenderPassCreateInfo2, create_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkSubpassDescription2, subpasses,
                           pCreateInfo->subpassCount);
   VK_MULTIALLOC_DECL(&ma, VkAttachmentDescription2, attachments,
                           pCreateInfo->attachmentCount);
   VK_MULTIALLOC_DECL(&ma, VkSubpassDependency2, dependencies,
                           pCreateInfo->dependencyCount);
   VK_MULTIALLOC_DECL(&ma, VkAttachmentReference2, references,
                           reference_count);
   if (!vk_multialloc_alloc2(&ma, &device->alloc, pAllocator,
                             VK_SYSTEM_ALLOCATION_SCOPE_COMMAND))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkAttachmentReference2 *reference_ptr = references;

   const VkRenderPassMultiviewCreateInfo *multiview_info = NULL;
   const VkRenderPassInputAttachmentAspectCreateInfo *aspect_info = NULL;
   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
         aspect_info = (const VkRenderPassInputAttachmentAspectCreateInfo *)ext;
         /* We don't care about this information */
         break;

      case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
         multiview_info = (const VkRenderPassMultiviewCreateInfo*) ext;
         break;

      case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
         /* pass this through to CreateRenderPass2 */
         break;

      default:
         mesa_logd("%s: ignored VkStructureType %u\n", __func__, ext->sType);
         break;
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
      attachments[i] = (VkAttachmentDescription2) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pAttachments[i].flags,
         .format = pCreateInfo->pAttachments[i].format,
         .samples = pCreateInfo->pAttachments[i].samples,
         .loadOp = pCreateInfo->pAttachments[i].loadOp,
         .storeOp = pCreateInfo->pAttachments[i].storeOp,
         .stencilLoadOp = pCreateInfo->pAttachments[i].stencilLoadOp,
         .stencilStoreOp = pCreateInfo->pAttachments[i].stencilStoreOp,
         .initialLayout = pCreateInfo->pAttachments[i].initialLayout,
         .finalLayout = pCreateInfo->pAttachments[i].finalLayout,
      };
   }

   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      subpasses[i] = (VkSubpassDescription2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
         .pNext = NULL,
         .flags = pCreateInfo->pSubpasses[i].flags,
         .pipelineBindPoint = pCreateInfo->pSubpasses[i].pipelineBindPoint,
         .viewMask = 0,
         .inputAttachmentCount = pCreateInfo->pSubpasses[i].inputAttachmentCount,
         .colorAttachmentCount = pCreateInfo->pSubpasses[i].colorAttachmentCount,
         .preserveAttachmentCount = pCreateInfo->pSubpasses[i].preserveAttachmentCount,
         .pPreserveAttachments = pCreateInfo->pSubpasses[i].pPreserveAttachments,
      };

      if (multiview_info && multiview_info->subpassCount) {
         assert(multiview_info->subpassCount == pCreateInfo->subpassCount);
         subpasses[i].viewMask = multiview_info->pViewMasks[i];
      }

      subpasses[i].pInputAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           subpasses[i].inputAttachmentCount,
                           pCreateInfo->pSubpasses[i].pInputAttachments,
                           pCreateInfo, true);
      subpasses[i].pColorAttachments = reference_ptr;
      translate_references(&reference_ptr,
                           subpasses[i].colorAttachmentCount,
                           pCreateInfo->pSubpasses[i].pColorAttachments,
                           pCreateInfo, false);
      subpasses[i].pResolveAttachments = NULL;
      if (pCreateInfo->pSubpasses[i].pResolveAttachments) {
         subpasses[i].pResolveAttachments = reference_ptr;
         translate_references(&reference_ptr,
                              subpasses[i].colorAttachmentCount,
                              pCreateInfo->pSubpasses[i].pResolveAttachments,
                              pCreateInfo, false);
      }
      subpasses[i].pDepthStencilAttachment = NULL;
      if (pCreateInfo->pSubpasses[i].pDepthStencilAttachment) {
         subpasses[i].pDepthStencilAttachment = reference_ptr;
         translate_references(&reference_ptr, 1,
                              pCreateInfo->pSubpasses[i].pDepthStencilAttachment,
                              pCreateInfo, false);
      }
   }

   assert(reference_ptr == references + reference_count);

   if (aspect_info != NULL) {
      for (uint32_t i = 0; i < aspect_info->aspectReferenceCount; i++) {
         const VkInputAttachmentAspectReference *ref =
            &aspect_info->pAspectReferences[i];

         assert(ref->subpass < pCreateInfo->subpassCount);
         VkSubpassDescription2 *subpass = &subpasses[ref->subpass];

         assert(ref->inputAttachmentIndex < subpass->inputAttachmentCount);
         VkAttachmentReference2 *att = (VkAttachmentReference2 *)
            &subpass->pInputAttachments[ref->inputAttachmentIndex];

         att->aspectMask = ref->aspectMask;
      }
   }

   for (uint32_t i = 0; i < pCreateInfo->dependencyCount; i++) {
      dependencies[i] = (VkSubpassDependency2) {
         .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
         .pNext = NULL,
         .srcSubpass = pCreateInfo->pDependencies[i].srcSubpass,
         .dstSubpass = pCreateInfo->pDependencies[i].dstSubpass,
         .srcStageMask = pCreateInfo->pDependencies[i].srcStageMask,
         .dstStageMask = pCreateInfo->pDependencies[i].dstStageMask,
         .srcAccessMask = pCreateInfo->pDependencies[i].srcAccessMask,
         .dstAccessMask = pCreateInfo->pDependencies[i].dstAccessMask,
         .dependencyFlags = pCreateInfo->pDependencies[i].dependencyFlags,
         .viewOffset = 0,
      };

      if (multiview_info && multiview_info->dependencyCount) {
         assert(multiview_info->dependencyCount == pCreateInfo->dependencyCount);
         dependencies[i].viewOffset = multiview_info->pViewOffsets[i];
      }
   }

   *create_info = (VkRenderPassCreateInfo2) {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
      .pNext = pCreateInfo->pNext,
      .flags = pCreateInfo->flags,
      .attachmentCount = pCreateInfo->attachmentCount,
      .pAttachments = attachments,
      .subpassCount = pCreateInfo->subpassCount,
      .pSubpasses = subpasses,
      .dependencyCount = pCreateInfo->dependencyCount,
      .pDependencies = dependencies,
   };

   if (multiview_info && multiview_info->correlationMaskCount > 0) {
      create_info->correlatedViewMaskCount = multiview_info->correlationMaskCount;
      create_info->pCorrelatedViewMasks = multiview_info->pCorrelationMasks;
   }

   VkResult result =
      device->dispatch_table.CreateRenderPass2(_device, create_info,
                                               pAllocator, pRenderPass);

   vk_free2(&device->alloc, pAllocator, create_info);

   return result;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                             const VkRenderPassBeginInfo* pRenderPassBegin,
                             VkSubpassContents contents)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassBeginInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents,
   };

   disp->device->dispatch_table.CmdBeginRenderPass2(commandBuffer,
                                                    pRenderPassBegin, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassEndInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   };

   disp->device->dispatch_table.CmdEndRenderPass2(commandBuffer, &info);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdNextSubpass(VkCommandBuffer commandBuffer,
                         VkSubpassContents contents)
{
   /* We don't have a vk_command_buffer object but we can assume, since we're
    * using common dispatch, that it's a vk_object of some sort.
    */
   struct vk_object_base *disp = (struct vk_object_base *)commandBuffer;

   VkSubpassBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,
      .contents = contents,
   };

   VkSubpassEndInfo end_info = {
      .sType = VK_STRUCTURE_TYPE_SUBPASS_END_INFO,
   };

   disp->device->dispatch_table.CmdNextSubpass2(commandBuffer, &begin_info,
                                                &end_info);
}

static unsigned
num_subpass_attachments2(const VkSubpassDescription2 *desc)
{
   bool has_depth_stencil_attachment =
      desc->pDepthStencilAttachment != NULL &&
      desc->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;

   const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
      vk_find_struct_const(desc->pNext,
                           SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

   bool has_depth_stencil_resolve_attachment =
      ds_resolve != NULL && ds_resolve->pDepthStencilResolveAttachment &&
      ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED;

   const VkFragmentShadingRateAttachmentInfoKHR *fsr_att_info =
      vk_find_struct_const(desc->pNext,
                           FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);

   bool has_fragment_shading_rate_attachment =
      fsr_att_info && fsr_att_info->pFragmentShadingRateAttachment &&
      fsr_att_info->pFragmentShadingRateAttachment->attachment != VK_ATTACHMENT_UNUSED;

   return desc->inputAttachmentCount +
          desc->colorAttachmentCount +
          (desc->pResolveAttachments ? desc->colorAttachmentCount : 0) +
          has_depth_stencil_attachment +
          has_depth_stencil_resolve_attachment +
          has_fragment_shading_rate_attachment;
}

static void
vk_render_pass_attachment_init(struct vk_render_pass_attachment *att,
                               const VkAttachmentDescription2 *desc)
{
   *att = (struct vk_render_pass_attachment) {
      .format                 = desc->format,
      .aspects                = vk_format_aspects(desc->format),
      .samples                = desc->samples,
      .view_mask              = 0,
      .load_op                = desc->loadOp,
      .store_op               = desc->storeOp,
      .stencil_load_op        = desc->stencilLoadOp,
      .stencil_store_op       = desc->stencilStoreOp,
      .initial_layout         = desc->initialLayout,
      .final_layout           = desc->finalLayout,
      .initial_stencil_layout = vk_att_desc_stencil_layout(desc, false),
      .final_stencil_layout   = vk_att_desc_stencil_layout(desc, true),
   };
}

static void
vk_subpass_attachment_init(struct vk_subpass_attachment *att,
                           struct vk_render_pass *pass,
                           uint32_t subpass_idx,
                           const VkAttachmentReference2 *ref,
                           const VkAttachmentDescription2 *attachments,
                           VkImageUsageFlagBits usage)
{
   if (ref->attachment >= pass->attachment_count) {
      assert(ref->attachment == VK_ATTACHMENT_UNUSED);
      *att = (struct vk_subpass_attachment) {
         .attachment = VK_ATTACHMENT_UNUSED,
      };
      return;
   }

   struct vk_render_pass_attachment *pass_att =
      &pass->attachments[ref->attachment];

   *att = (struct vk_subpass_attachment) {
      .attachment =     ref->attachment,
      .aspects =        vk_format_aspects(pass_att->format),
      .usage =          usage,
      .layout =         ref->layout,
      .stencil_layout = vk_att_ref_stencil_layout(ref, attachments),
   };

   switch (usage) {
   case VK_IMAGE_USAGE_TRANSFER_DST_BIT:
      break; /* No special aspect requirements */

   case VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:
      /* From the Vulkan 1.2.184 spec:
       *
       *    "aspectMask is ignored when this structure is used to describe
       *    anything other than an input attachment reference."
       */
      assert(!(ref->aspectMask & ~att->aspects));
      att->aspects = ref->aspectMask;
      break;

   case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:
   case VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR:
      assert(att->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
      break;

   case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
      assert(!(att->aspects & ~(VK_IMAGE_ASPECT_DEPTH_BIT |
                                VK_IMAGE_ASPECT_STENCIL_BIT)));
      break;

   default:
      unreachable("Invalid subpass attachment usage");
   }
}

static void
vk_subpass_attachment_link_resolve(struct vk_subpass_attachment *att,
                                   struct vk_subpass_attachment *resolve,
                                   const VkRenderPassCreateInfo2 *info)
{
   if (resolve->attachment == VK_ATTACHMENT_UNUSED)
      return;

   assert(att->attachment != VK_ATTACHMENT_UNUSED);
   att->resolve = resolve;
}

static void
vk_subpass_init_ial(struct vk_subpass *subpass)
{
   subpass->ial.depth = VK_ATTACHMENT_UNUSED;
   subpass->ial.stencil = VK_ATTACHMENT_UNUSED;
   for (uint32_t i = 0; i < ARRAY_SIZE(subpass->ial.colors); i++)
      subpass->ial.colors[i] = VK_ATTACHMENT_UNUSED;

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      uint32_t col_att = subpass->color_attachments[i].attachment;

      if (col_att == VK_ATTACHMENT_UNUSED)
         continue;

      for (uint32_t j = 0; j < subpass->input_count; j++) {
         uint32_t input_att = subpass->input_attachments[j].attachment;

         if (input_att == col_att)
            subpass->ial.colors[i] = j;
      }
   }

   uint32_t ds_att = subpass->depth_stencil_attachment
                        ? subpass->depth_stencil_attachment->attachment
                        : VK_ATTACHMENT_UNUSED;
   if (ds_att != VK_ATTACHMENT_UNUSED) {
      const VkImageAspectFlags aspects =
         subpass->depth_stencil_attachment->aspects;

      for (uint32_t j = 0; j < subpass->input_count; j++) {
         uint32_t input_att = subpass->input_attachments[j].attachment;

         if (input_att == ds_att) {
            if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
               subpass->ial.depth = j;
            if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
               subpass->ial.stencil = j;
         }
      }
   }
}

static void
vk_subpass_init_info(struct vk_subpass *subpass,
                     const VkFormat *color_formats,
                     const VkSampleCountFlagBits *color_samples,
                     VkFormat depth_format, VkFormat stencil_format,
                     VkSampleCountFlagBits depth_stencil_samples,
                     const VkMultisampledRenderToSingleSampledInfoEXT *mrtss)
{
   subpass->sample_count_info_amd = (VkAttachmentSampleCountInfoAMD) {
      .sType = VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,
      .pNext = NULL,
      .colorAttachmentCount = subpass->color_count,
      .pColorAttachmentSamples = color_samples,
      .depthStencilAttachmentSamples = depth_stencil_samples,
   };

   vk_subpass_init_ial(subpass);
   subpass->ial.info = (VkRenderingInputAttachmentIndexInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
      .pNext = &subpass->sample_count_info_amd,
      .colorAttachmentCount = subpass->color_count,
      .pColorAttachmentInputIndices = subpass->ial.colors,
      .pDepthInputAttachmentIndex = &subpass->ial.depth,
      .pDepthInputAttachmentIndex = &subpass->ial.stencil,
   };

   /* Color remapping table should be initialized by the caller. */
   subpass->cal.info = (VkRenderingAttachmentLocationInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
      .pNext = &subpass->ial.info,
      .colorAttachmentCount = subpass->color_count,
      .pColorAttachmentLocations = subpass->cal.colors,
   };

   subpass->pipeline_info = (VkPipelineRenderingCreateInfo) {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .pNext = &subpass->cal.info,
      .viewMask = subpass->view_mask,
      .colorAttachmentCount = subpass->color_count,
      .pColorAttachmentFormats = color_formats,
      .depthAttachmentFormat = depth_format,
      .stencilAttachmentFormat = stencil_format,
   };

   VkSampleCountFlagBits samples = 0;

   if (depth_format != VK_FORMAT_UNDEFINED ||
       stencil_format != VK_FORMAT_UNDEFINED)
      samples |= depth_stencil_samples;

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      if (color_formats[i] != VK_FORMAT_UNDEFINED)
         samples |= color_samples[i];
   }

   subpass->inheritance_info = (VkCommandBufferInheritanceRenderingInfo) {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
      .pNext = &subpass->cal.info,
      /* If we're inheriting, the contents are clearly in secondaries */
      .flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
      .viewMask = subpass->view_mask,
      .colorAttachmentCount = subpass->color_count,
      .pColorAttachmentFormats = color_formats,
      .depthAttachmentFormat = depth_format,
      .stencilAttachmentFormat = stencil_format,
      .rasterizationSamples = samples,
   };

   if (mrtss) {
      assert(mrtss->multisampledRenderToSingleSampledEnable);
      subpass->mrtss = (VkMultisampledRenderToSingleSampledInfoEXT) {
         .sType = VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT,
         .multisampledRenderToSingleSampledEnable = VK_TRUE,
         .rasterizationSamples = mrtss->rasterizationSamples,
      };
   }
}

static VkResult
vk_subpass_create(const VkRenderPassCreateInfo2 *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  struct vk_render_pass *pass,
                  uint32_t subpass_idx)
{
   struct vk_device *device = pass->base.device;
   uint32_t subpass_attachment_count =
      num_subpass_attachments2(&pCreateInfo->pSubpasses[subpass_idx]);
   uint32_t subpass_color_attachment_count =
      pCreateInfo->pSubpasses[subpass_idx].colorAttachmentCount;

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass, subpass, 1);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass_attachment, subpass_attachments,
                      subpass_attachment_count);
   VK_MULTIALLOC_DECL(&ma, VkFormat, subpass_color_formats,
                      subpass_color_attachment_count);
   VK_MULTIALLOC_DECL(&ma, VkSampleCountFlagBits, subpass_color_samples,
                      subpass_color_attachment_count);

   if (!vk_multialloc_zalloc2(&ma, &device->alloc, pAllocator,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct vk_subpass_attachment *next_subpass_attachment = subpass_attachments;
   const VkSubpassDescription2 *desc = &pCreateInfo->pSubpasses[subpass_idx];
   const VkMultisampledRenderToSingleSampledInfoEXT *mrtss =
         vk_find_struct_const(desc->pNext, MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT);
   if (mrtss && !mrtss->multisampledRenderToSingleSampledEnable)
      mrtss = NULL;

   if (device->enabled_features.legacyDithering) {
      subpass->legacy_dithering_enabled =
         desc->flags & VK_SUBPASS_DESCRIPTION_ENABLE_LEGACY_DITHERING_BIT_EXT;
   }

   /* From the Vulkan 1.3.204 spec:
    *
    *    VUID-VkRenderPassCreateInfo2-viewMask-03058
    *
    *    "The VkSubpassDescription2::viewMask member of all elements of
    *    pSubpasses must either all be 0, or all not be 0"
    */
   if (desc->viewMask)
      pass->is_multiview = true;
   assert(pass->is_multiview == (desc->viewMask != 0));

   /* For all view masks in the vk_render_pass data structure, we use a
    * mask of 1 for non-multiview instead of a mask of 0.
    */
   subpass->view_mask = desc->viewMask ? desc->viewMask : 1;
   pass->view_mask |= subpass->view_mask;

   subpass->input_count = desc->inputAttachmentCount;
   if (desc->inputAttachmentCount > 0) {
      subpass->input_attachments = next_subpass_attachment;
      next_subpass_attachment += desc->inputAttachmentCount;

      for (uint32_t a = 0; a < desc->inputAttachmentCount; a++) {
         vk_subpass_attachment_init(&subpass->input_attachments[a],
                                    pass, subpass_idx,
                                    &desc->pInputAttachments[a],
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      }
   }

   subpass->color_count = desc->colorAttachmentCount;
   if (desc->colorAttachmentCount > 0) {
      subpass->color_attachments = next_subpass_attachment;
      next_subpass_attachment += desc->colorAttachmentCount;

      for (uint32_t a = 0; a < desc->colorAttachmentCount; a++) {
         vk_subpass_attachment_init(&subpass->color_attachments[a],
                                    pass, subpass_idx,
                                    &desc->pColorAttachments[a],
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
      }
   }

   if (desc->pResolveAttachments) {
      subpass->color_resolve_count = desc->colorAttachmentCount;
      subpass->color_resolve_attachments = next_subpass_attachment;
      next_subpass_attachment += desc->colorAttachmentCount;

      for (uint32_t a = 0; a < desc->colorAttachmentCount; a++) {
         vk_subpass_attachment_init(&subpass->color_resolve_attachments[a],
                                    pass, subpass_idx,
                                    &desc->pResolveAttachments[a],
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
         vk_subpass_attachment_link_resolve(&subpass->color_attachments[a],
                                            &subpass->color_resolve_attachments[a],
                                            pCreateInfo);
      }
   }

   if (desc->pDepthStencilAttachment &&
       desc->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED) {
      subpass->depth_stencil_attachment = next_subpass_attachment++;

      vk_subpass_attachment_init(subpass->depth_stencil_attachment,
                                 pass, subpass_idx,
                                 desc->pDepthStencilAttachment,
                                 pCreateInfo->pAttachments,
                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
   }

   const VkSubpassDescriptionDepthStencilResolve *ds_resolve =
      vk_find_struct_const(desc->pNext,
                           SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE);

   if (ds_resolve) {
      if (ds_resolve->pDepthStencilResolveAttachment &&
          ds_resolve->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED) {
         subpass->depth_stencil_resolve_attachment = next_subpass_attachment++;

         vk_subpass_attachment_init(subpass->depth_stencil_resolve_attachment,
                                    pass, subpass_idx,
                                    ds_resolve->pDepthStencilResolveAttachment,
                                    pCreateInfo->pAttachments,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
         vk_subpass_attachment_link_resolve(subpass->depth_stencil_attachment,
                                            subpass->depth_stencil_resolve_attachment,
                                            pCreateInfo);
      }
      if (subpass->depth_stencil_resolve_attachment || mrtss) {
         /* From the Vulkan 1.3.204 spec:
          *
          *    VUID-VkSubpassDescriptionDepthStencilResolve-pDepthStencilResolveAttachment-03178
          *
          *    "If pDepthStencilResolveAttachment is not NULL and does not
          *    have the value VK_ATTACHMENT_UNUSED, depthResolveMode and
          *    stencilResolveMode must not both be VK_RESOLVE_MODE_NONE"
          */
         assert(ds_resolve->depthResolveMode != VK_RESOLVE_MODE_NONE ||
                ds_resolve->stencilResolveMode != VK_RESOLVE_MODE_NONE);

         subpass->depth_resolve_mode = ds_resolve->depthResolveMode;
         subpass->stencil_resolve_mode = ds_resolve->stencilResolveMode;
      }
   }

   const VkFragmentShadingRateAttachmentInfoKHR *fsr_att_info =
      vk_find_struct_const(desc->pNext,
                           FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR);

   if (fsr_att_info && fsr_att_info->pFragmentShadingRateAttachment &&
       fsr_att_info->pFragmentShadingRateAttachment->attachment != VK_ATTACHMENT_UNUSED) {
      subpass->fragment_shading_rate_attachment = next_subpass_attachment++;
      vk_subpass_attachment_init(subpass->fragment_shading_rate_attachment,
                                 pass, subpass_idx,
                                 fsr_att_info->pFragmentShadingRateAttachment,
                                 pCreateInfo->pAttachments,
                                 VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
      subpass->fragment_shading_rate_attachment_texel_size =
         fsr_att_info->shadingRateAttachmentTexelSize;
      subpass->pipeline_flags |=
         VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
   }

   /* Figure out any self-dependencies */
   assert(desc->colorAttachmentCount <= 32);
   for (uint32_t a = 0; a < desc->inputAttachmentCount; a++) {
      if (desc->pInputAttachments[a].attachment == VK_ATTACHMENT_UNUSED)
         continue;

      for (uint32_t c = 0; c < desc->colorAttachmentCount; c++) {
         if (desc->pColorAttachments[c].attachment ==
             desc->pInputAttachments[a].attachment) {
            subpass->input_attachments[a].layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->color_attachments[c].layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->pipeline_flags |=
               VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
         }
      }

      if (desc->pDepthStencilAttachment != NULL &&
          desc->pDepthStencilAttachment->attachment ==
             desc->pInputAttachments[a].attachment) {
         VkImageAspectFlags aspects =
            subpass->input_attachments[a].aspects;
         if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
            subpass->input_attachments[a].layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->depth_stencil_attachment->layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->pipeline_flags |=
               VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
         }
         if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
            subpass->input_attachments[a].stencil_layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->depth_stencil_attachment->stencil_layout =
               VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
            subpass->pipeline_flags |=
               VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
         }
      }
   }

   VkFormat *color_formats = NULL;
   VkSampleCountFlagBits *color_samples = NULL;
   if (desc->colorAttachmentCount > 0) {
      color_formats = subpass_color_formats;
      color_samples = subpass_color_samples;
      for (uint32_t a = 0; a < desc->colorAttachmentCount; a++) {
         const VkAttachmentReference2 *ref = &desc->pColorAttachments[a];
         if (ref->attachment >= pCreateInfo->attachmentCount) {
            color_formats[a] = VK_FORMAT_UNDEFINED;
            color_samples[a] = VK_SAMPLE_COUNT_1_BIT;
         } else {
            const VkAttachmentDescription2 *att =
               &pCreateInfo->pAttachments[ref->attachment];

            color_formats[a] = att->format;
            color_samples[a] = att->samples;
         }
      }
   }

   VkFormat depth_format = VK_FORMAT_UNDEFINED;
   VkFormat stencil_format = VK_FORMAT_UNDEFINED;
   VkSampleCountFlagBits depth_stencil_samples = VK_SAMPLE_COUNT_1_BIT;
   if (desc->pDepthStencilAttachment != NULL) {
      const VkAttachmentReference2 *ref = desc->pDepthStencilAttachment;
      if (ref->attachment < pCreateInfo->attachmentCount) {
         const VkAttachmentDescription2 *att =
            &pCreateInfo->pAttachments[ref->attachment];

         if (vk_format_has_depth(att->format))
            depth_format = att->format;
         if (vk_format_has_stencil(att->format))
            stencil_format = att->format;

         depth_stencil_samples = att->samples;
      }
   }

   /* Identity mapping by default. */
   for (uint32_t i = 0; i < ARRAY_SIZE(subpass->cal.colors); i++)
      subpass->cal.colors[i] = i;

   vk_subpass_init_info(subpass, color_formats, color_samples,
                        depth_format, stencil_format, depth_stencil_samples,
                        mrtss);

   pass->subpasses[subpass_idx] = subpass;
   return VK_SUCCESS;
}

static void
init_subpass_merging_ctx(struct vk_render_pass *pass, uint32_t first_subpass,
                         struct vk_subpass_merging_ctx *ctx)
{
   assert(first_subpass < pass->subpass_count);

   const struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, first_subpass);

   *ctx = (struct vk_subpass_merging_ctx){
      .first_subpass = first_subpass,
      .last_subpass = first_subpass,
      .attachments = {
         .colors = {
            {VK_ATTACHMENT_UNUSED}, {VK_ATTACHMENT_UNUSED},
            {VK_ATTACHMENT_UNUSED}, {VK_ATTACHMENT_UNUSED},
            {VK_ATTACHMENT_UNUSED}, {VK_ATTACHMENT_UNUSED},
            {VK_ATTACHMENT_UNUSED}, {VK_ATTACHMENT_UNUSED},
         },
         .depth = {VK_ATTACHMENT_UNUSED},
         .stencil = {VK_ATTACHMENT_UNUSED},
      },
   };

   assert(subpass->color_count <= ARRAY_SIZE(ctx->attachments.colors));

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      if (subpass->color_attachments[i].attachment == VK_ATTACHMENT_UNUSED)
         continue;

      ctx->attachments.colors[i] = (struct vk_subpass_merging_attachment_ref){
         .subpass = first_subpass,
         .access_count = 1,
         .index = i,
      };

      ctx->attachments.used_color_mask |= BITFIELD_BIT(i);
   }

   if (subpass->depth_stencil_attachment &&
       (subpass->depth_stencil_attachment->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      ctx->attachments.depth = (struct vk_subpass_merging_attachment_ref){
         .subpass = first_subpass,
         .access_count = 1,
      };
   }

   if (subpass->depth_stencil_attachment &&
       (subpass->depth_stencil_attachment->aspects &
        VK_IMAGE_ASPECT_STENCIL_BIT)) {
      ctx->attachments.stencil = (struct vk_subpass_merging_attachment_ref){
         .subpass = first_subpass,
         .access_count = 1,
      };
   }
}

static uint32_t
subpass_merging_ctx_get_ds_attachment(struct vk_render_pass *pass,
                                      const struct vk_subpass_merging_ctx *ctx,
                                      VkImageAspectFlagBits aspect)
{
   struct vk_subpass_merging_attachment_ref ref = {VK_ATTACHMENT_UNUSED};

   if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
      ref = ctx->attachments.depth;
   else if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
      ref = ctx->attachments.stencil;

   if (ref.subpass == VK_ATTACHMENT_UNUSED)
      return VK_ATTACHMENT_UNUSED;

   struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, ref.subpass);

   return subpass->depth_stencil_attachment->attachment;
}

static uint32_t
subpass_merging_ctx_get_col_attachment(struct vk_render_pass *pass,
                                       const struct vk_subpass_merging_ctx *ctx,
                                       uint32_t col_index)
{
   struct vk_subpass_merging_attachment_ref ref =
      ctx->attachments.colors[col_index];

   if (ref.subpass == VK_ATTACHMENT_UNUSED)
      return VK_ATTACHMENT_UNUSED;

   struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, ref.subpass);

   return subpass->color_attachments[ref.index].attachment;
}

static uint32_t
merged_subpasses_attachment_count(struct vk_render_pass *pass,
                                  const struct vk_subpass_merging_ctx *ctx)
{
   uint32_t color_count = util_last_bit(ctx->attachments.used_color_mask);
   bool has_ds = false, has_ds_reslv = false, has_col_reslv = false;
   uint32_t input_count = 0;

   for (uint32_t i = ctx->first_subpass;  i <= ctx->last_subpass; i++) {
      struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, i);

      input_count += subpass->input_count;
      has_ds |= subpass->depth_stencil_attachment != NULL;
      has_ds_reslv |= subpass->depth_stencil_resolve_attachment != NULL;
      has_col_reslv |= subpass->color_resolve_count > 0;
   }

   return color_count + (has_col_reslv ? color_count : 0) + input_count +
          (has_ds ? 1 : 0) + (has_ds_reslv ? 1 : 0);
}

VkResult
vk_render_pass_merge_subpasses(struct vk_render_pass *pass,
                               const VkAllocationCallbacks *alloc,
                               const struct vk_subpass_merging_ctx *ctx)
{
   /* Bail out early if there's nothing to merge. */
   if (ctx->first_subpass == ctx->last_subpass)
      return VK_SUCCESS;

   uint32_t subpass_count = ctx->last_subpass + 1 - ctx->first_subpass;
   uint32_t att_count = merged_subpasses_attachment_count(pass, ctx);
   uint32_t color_count = util_last_bit(ctx->attachments.used_color_mask);
   VkSampleCountFlags depth_stencil_samples = VK_SAMPLE_COUNT_1_BIT;
   VkFormat depth_format = VK_FORMAT_UNDEFINED;
   VkFormat stencil_format = VK_FORMAT_UNDEFINED;
   const VkMultisampledRenderToSingleSampledInfoEXT *mrtss = NULL;

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass, subpasses, subpass_count);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass_attachment, atts, att_count);
   VK_MULTIALLOC_DECL(&ma, VkFormat, color_formats, color_count);
   VK_MULTIALLOC_DECL(&ma, VkSampleCountFlagBits, color_samples, color_count);

   if (!vk_multialloc_zalloc2(&ma, &pass->base.device->alloc, alloc,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct vk_subpass subpass_tmpl = {0};

   subpass_tmpl.color_count =
      util_last_bit(ctx->attachments.used_color_mask);
   subpass_tmpl.color_attachments = atts;
   atts += subpass_tmpl.color_count;

   for (uint32_t i = 0; i < color_count; i++) {
      subpass_tmpl.color_attachments[i] = (struct vk_subpass_attachment){
         .attachment = VK_ATTACHMENT_UNUSED,
      };
      color_formats[i] = VK_FORMAT_UNDEFINED;
      color_samples[i] = VK_SAMPLE_COUNT_1_BIT;
   }

   uint32_t ds_att_subpass = VK_ATTACHMENT_UNUSED;

   if (ctx->attachments.depth.subpass != VK_ATTACHMENT_UNUSED)
      ds_att_subpass = ctx->attachments.depth.subpass;
   else if (ctx->attachments.stencil.subpass != VK_ATTACHMENT_UNUSED)
      ds_att_subpass = ctx->attachments.stencil.subpass;

   struct vk_subpass *ds_first_subpass =
      ds_att_subpass != VK_ATTACHMENT_UNUSED ?
      vk_render_pass_get_subpass(pass, ds_att_subpass) : NULL;
   struct vk_subpass_attachment *ds_att =
      ds_first_subpass ? ds_first_subpass->depth_stencil_attachment : NULL;

   if (ds_att) {
      subpass_tmpl.depth_stencil_attachment = atts++;
      *subpass_tmpl.depth_stencil_attachment = *ds_att;
      depth_stencil_samples = pass->attachments[ds_att->attachment].samples;

      if (ctx->attachments.depth.subpass != VK_ATTACHMENT_UNUSED)
         depth_format = pass->attachments[ds_att->attachment].format;
      if (ctx->attachments.stencil.subpass != VK_ATTACHMENT_UNUSED)
         stencil_format = pass->attachments[ds_att->attachment].format;
   }

   for (uint32_t i = ctx->first_subpass;  i <= ctx->last_subpass; i++) {
      struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, i);

      if (i == ctx->first_subpass &&
          subpass->mrtss.multisampledRenderToSingleSampledEnable)
         mrtss = &subpass->mrtss;

      subpass_tmpl.view_mask |= subpass->view_mask;

      if (subpass->color_resolve_count > 0 &&
          subpass_tmpl.color_resolve_count == 0) {
         subpass_tmpl.color_resolve_count = subpass->color_count;
         subpass_tmpl.color_resolve_attachments = atts;
         atts += subpass->color_count;
         for (uint32_t j = 0; j < color_count; j++) {
            subpass->color_resolve_attachments[j] = (struct vk_subpass_attachment){
               .attachment = VK_ATTACHMENT_UNUSED,
            };
         }
      }

      if (subpass->depth_stencil_resolve_attachment &&
          !subpass_tmpl.depth_stencil_resolve_attachment) {
         subpass_tmpl.depth_stencil_resolve_attachment = atts++;
         *subpass_tmpl.depth_stencil_resolve_attachment =
            *subpass->depth_stencil_resolve_attachment;
         subpass_tmpl.depth_stencil_attachment->resolve =
            subpass_tmpl.depth_stencil_resolve_attachment;
      }

      if (subpass->depth_resolve_mode != VK_RESOLVE_MODE_NONE)
         subpass_tmpl.depth_resolve_mode = subpass->depth_resolve_mode;

      if (subpass->stencil_resolve_mode != VK_RESOLVE_MODE_NONE)
         subpass_tmpl.stencil_resolve_mode = subpass->stencil_resolve_mode;
   }

   for (uint32_t i = 0; i < color_count; i++) {
      struct vk_subpass_merging_attachment_ref ref = ctx->attachments.colors[i];

      if (ref.subpass == VK_ATTACHMENT_UNUSED)
         continue;

      const struct vk_subpass *subpass =
         vk_render_pass_get_subpass(pass, ref.subpass);

      subpass_tmpl.color_attachments[i] =
         subpass->color_attachments[ref.index];

      uint32_t att_idx = subpass_tmpl.color_attachments[i].attachment;
      const struct vk_render_pass_attachment *att = &pass->attachments[att_idx];

      color_formats[i] = att->format;
      color_samples[i] = att->samples;

      if (subpass->color_resolve_attachments) {
         subpass_tmpl.color_resolve_attachments[i] =
            subpass->color_resolve_attachments[ref.index];
         subpass_tmpl.color_attachments[i].resolve =
            &subpass_tmpl.color_resolve_attachments[i];
      }
   }

   for (uint32_t i = ctx->first_subpass;  i <= ctx->last_subpass; i++) {
      const struct vk_subpass *orig_subpass = vk_render_pass_get_subpass(pass, i);

      for (uint32_t j = 0; j < orig_subpass->input_count; j++) {
         struct vk_subpass_attachment *orig_ia =
            &orig_subpass->input_attachments[j];
         if (orig_ia->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         for (uint32_t k = 0; k < color_count; k++) {
            if (orig_ia->attachment ==
                   subpass_tmpl.color_attachments[k].attachment) {
               subpass_tmpl.pipeline_flags |=
                  VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
               break;
            }
         }

         if (ds_att && orig_ia->attachment == ds_att->attachment &&
             (ds_att->aspects &
              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
            subpass_tmpl.pipeline_flags |=
               VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
         }
      }
   }

   for (uint32_t i = ctx->first_subpass;  i <= ctx->last_subpass; i++) {
      const struct vk_subpass *orig_subpass = vk_render_pass_get_subpass(pass, i);
      struct vk_subpass *new_subpass = &subpasses[i - ctx->first_subpass];

      *new_subpass = subpass_tmpl;
      if (i == ctx->first_subpass)
         new_subpass->merged = VK_SUBPASS_MERGED_FIRST;
      else if (i == ctx->last_subpass)
         new_subpass->merged = VK_SUBPASS_MERGED_LAST;
      else
         new_subpass->merged = VK_SUBPASS_MERGED_MID;

      new_subpass->cal.info.colorAttachmentCount = color_count;
      new_subpass->cal.info.pColorAttachmentLocations = new_subpass->cal.colors;
      for (uint32_t k = 0; k < color_count; k++)
         new_subpass->cal.colors[k] = VK_ATTACHMENT_UNUSED;

      for (uint32_t j = 0; j < orig_subpass->color_count; j++) {
         struct vk_subpass_attachment *orig_ca =
            &orig_subpass->color_attachments[j];
         if (orig_ca->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         for (uint32_t k = 0; k < color_count; k++) {
           if (orig_ca->attachment ==
                  subpass_tmpl.color_attachments[k].attachment) {
               new_subpass->cal.colors[k] = j;
               break;
            }
         }
      }

      for (uint32_t j = 0; j < orig_subpass->input_count; j++) {
         struct vk_subpass_attachment *orig_ia =
            &orig_subpass->input_attachments[j];
         if (orig_ia->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         struct vk_subpass_attachment *new_ia = NULL;
         for (uint32_t k = 0; k < new_subpass->input_count && !new_ia; k++) {
            if (new_subpass->input_attachments[k].attachment ==
                   orig_ia->attachment) {
               new_ia = &new_subpass->input_attachments[k];
            }
         }

         if (new_ia) {
            new_ia->last_subpass |= orig_ia->last_subpass;
         } else {
            new_ia = atts++;
            *new_ia = *orig_ia;
            if (!new_subpass->input_count)
               new_subpass->input_attachments = new_ia;

            new_subpass->input_count++;
         }
      }

      vk_subpass_init_info(new_subpass, color_formats, color_samples,
                           depth_format, stencil_format, depth_stencil_samples,
                           mrtss);
   }

   for (uint32_t i = ctx->first_subpass;  i <= ctx->last_subpass; i++) {
      vk_free2(&pass->base.device->alloc, alloc, pass->subpasses[i]);
      pass->subpasses[i] = &subpasses[i - ctx->first_subpass];
   }

   return VK_SUCCESS;
}

static bool
can_merge_next_subpass(struct vk_render_pass *pass,
                       struct vk_subpass_merging_ctx *ctx)
{
   /* We reached the end of the pass. */
   if (ctx->last_subpass + 1 == pass->subpass_count)
      return false;

   const struct vk_physical_device *phys_dev = pass->base.device->physical;
   const struct vk_subpass *lsubpass =
      vk_render_pass_get_subpass(pass, ctx->last_subpass + 1);
   const struct vk_subpass *fsubpass =
      vk_render_pass_get_subpass(pass, ctx->first_subpass);

   /* FIXME: we don't merge subpasses when fragment shading rate is enabled to
    * keep things simple. */
   if (fsubpass->fragment_shading_rate_attachment ||
       lsubpass->fragment_shading_rate_attachment)
      return false;

   /* Resolve modes must match. */
   if (fsubpass->depth_resolve_mode != VK_RESOLVE_MODE_NONE &&
       lsubpass->depth_resolve_mode != VK_RESOLVE_MODE_NONE &&
       fsubpass->depth_resolve_mode != lsubpass->depth_resolve_mode)
      return false;

   if (fsubpass->stencil_resolve_mode != VK_RESOLVE_MODE_NONE &&
       lsubpass->stencil_resolve_mode != VK_RESOLVE_MODE_NONE &&
       fsubpass->stencil_resolve_mode != lsubpass->stencil_resolve_mode)
      return false;

   if (fsubpass->legacy_dithering_enabled != lsubpass->legacy_dithering_enabled)
      return false;

   if (fsubpass->mrtss.multisampledRenderToSingleSampledEnable !=
          lsubpass->mrtss.multisampledRenderToSingleSampledEnable ||
       fsubpass->mrtss.rasterizationSamples !=
          lsubpass->mrtss.rasterizationSamples)
      return false;

   /* First make sure all subpass dependencies for the range we consider
    * merging are FB-local. If one of them isn't, we can't merge. */
   for (uint32_t i = 0; i < pass->dependency_count; i++) {
      struct vk_subpass_dependency *dep = &pass->dependencies[i];

      if (dep->dst_subpass != ctx->last_subpass + 1 ||
          dep->src_subpass == VK_SUBPASS_EXTERNAL ||
          dep->dst_subpass == VK_SUBPASS_EXTERNAL ||
          dep->src_subpass < ctx->first_subpass ||
          dep->src_subpass > ctx->last_subpass + 1)
         continue;

      if (!vk_subpass_dependency_is_fb_local(dep->flags,
                                             dep->src_subpass,
                                             dep->dst_subpass,
                                             dep->src_stage_mask,
                                             dep->dst_stage_mask))
         return false;
   }

   uint32_t color_used_mask = 0;
   bool depth_used = false, stencil_used = false;
   uint32_t depth_att =
      lsubpass->depth_stencil_attachment &&
      (lsubpass->depth_stencil_attachment->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
         ? lsubpass->depth_stencil_attachment->attachment
         : VK_ATTACHMENT_UNUSED;
   uint32_t prev_depth_att =
      subpass_merging_ctx_get_ds_attachment(pass, ctx,
                                            VK_IMAGE_ASPECT_DEPTH_BIT);
   uint32_t stencil_att =
      lsubpass->depth_stencil_attachment &&
      (lsubpass->depth_stencil_attachment->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
         ? lsubpass->depth_stencil_attachment->attachment
         : VK_ATTACHMENT_UNUSED;
   uint32_t prev_stencil_att =
      subpass_merging_ctx_get_ds_attachment(pass, ctx,
                                            VK_IMAGE_ASPECT_STENCIL_BIT);

   /* If the depth/stencil attachments don't match we can't merge. */
   if (depth_att != VK_ATTACHMENT_UNUSED) {
      if ((prev_depth_att != VK_ATTACHMENT_UNUSED &&
           prev_depth_att != depth_att) ||
          (prev_stencil_att != VK_ATTACHMENT_UNUSED &&
           prev_stencil_att != depth_att))
         return false;

      depth_used = true;
   }

   if (stencil_att != VK_ATTACHMENT_UNUSED) {
      if ((prev_stencil_att != VK_ATTACHMENT_UNUSED &&
           prev_stencil_att != stencil_att) ||
          (prev_depth_att != VK_ATTACHMENT_UNUSED &&
           prev_depth_att != stencil_att))
         return false;

      stencil_used = true;
   }

   uint32_t free_colors =
      ~ctx->attachments.used_color_mask &
      BITFIELD_MASK(phys_dev->properties.maxColorAttachments);
   uint32_t missing_color_mask = 0;

   for (uint32_t i = 0; i < lsubpass->color_count; i++) {
      uint32_t col_att = lsubpass->color_attachments[i].attachment;

      if (col_att == VK_ATTACHMENT_UNUSED)
         break;

      bool found = false;
      u_foreach_bit(j, ctx->attachments.used_color_mask) {
         uint32_t existing_col_att =
            subpass_merging_ctx_get_col_attachment(pass, ctx, j);
         if (col_att == existing_col_att) {
            color_used_mask |= BITFIELD_BIT(j);
            found = true;
            break;
         }
      }

      if (!found)
         missing_color_mask |= BITFIELD_BIT(i);
   }

   /* If there's more colors than we have slots, we can't merge. */
   if (util_bitcount(missing_color_mask) > util_bitcount(free_colors))
      return false;

   u_foreach_bit(i, missing_color_mask) {
      uint32_t j = u_bit_scan(&free_colors);

      ctx->attachments.colors[j] = (struct vk_subpass_merging_attachment_ref){
         .subpass = ctx->last_subpass + 1,
         .index = i,
      };
      ctx->attachments.used_color_mask |= BITFIELD_BIT(j);
      color_used_mask |= BITFIELD_BIT(j);
   }

   if (depth_used &&
       ctx->attachments.depth.subpass == VK_ATTACHMENT_UNUSED)
      ctx->attachments.depth.subpass = ctx->last_subpass + 1;

   if (stencil_used &&
       ctx->attachments.stencil.subpass == VK_ATTACHMENT_UNUSED)
      ctx->attachments.stencil.subpass = ctx->last_subpass + 1;

   for (uint32_t i = 0; i < lsubpass->input_count; i++) {
      const struct vk_subpass_attachment *ia = &lsubpass->input_attachments[i];

      if (ia->attachment == VK_ATTACHMENT_UNUSED)
         continue;

      u_foreach_bit(j, ctx->attachments.used_color_mask) {
         struct vk_subpass_merging_attachment_ref ref =
            ctx->attachments.colors[j];
         const struct vk_subpass *ca_subpass =
            vk_render_pass_get_subpass(pass, ref.subpass);
         const struct vk_subpass_attachment *ca =
            &ca_subpass->color_attachments[ref.index];

         if (ca->attachment == ia->attachment)
            color_used_mask |= BITFIELD_BIT(j);
      }

      if (ctx->attachments.depth.subpass != VK_ATTACHMENT_UNUSED) {
         struct vk_subpass_merging_attachment_ref ref =
            ctx->attachments.depth;
         const struct vk_subpass *ca_subpass =
            vk_render_pass_get_subpass(pass, ref.subpass);
         const struct vk_subpass_attachment *ca =
            ca_subpass->depth_stencil_attachment;

         if (ca->attachment == ia->attachment)
            depth_used = true;
      }

      if (ctx->attachments.stencil.subpass != VK_ATTACHMENT_UNUSED) {
         struct vk_subpass_merging_attachment_ref ref =
            ctx->attachments.stencil;
         const struct vk_subpass *ca_subpass =
            vk_render_pass_get_subpass(pass, ref.subpass);
         const struct vk_subpass_attachment *ca =
            ca_subpass->depth_stencil_attachment;

         if (ca->attachment == ia->attachment)
            stencil_used = true;
      }
   }

   u_foreach_bit(i, color_used_mask) {
      ctx->attachments.colors[i].last_access = ctx->last_subpass + 1;
      ctx->attachments.colors[i].access_count++;
   }

   if (depth_used) {
      ctx->attachments.depth.last_access = ctx->last_subpass + 1;
      ctx->attachments.depth.access_count++;
   }

   if (stencil_used) {
      ctx->attachments.stencil.last_access = ctx->last_subpass + 1;
      ctx->attachments.stencil.access_count++;
   }

   return true;
}

void
vk_render_pass_next_mergeable_range(struct vk_render_pass *pass,
                                    uint32_t first_subpass,
                                    uint32_t last_subpass,
                                    struct vk_subpass_merging_ctx *ctx)
{
   init_subpass_merging_ctx(pass, first_subpass, ctx);
   while (ctx->last_subpass < last_subpass &&
          can_merge_next_subpass(pass, ctx)) {
      ctx->last_subpass++;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_CreateRenderPass2(VkDevice _device,
                            const VkRenderPassCreateInfo2 *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkRenderPass *pRenderPass)
{
   VK_FROM_HANDLE(vk_device, device, _device);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct vk_render_pass, pass, 1);
   VK_MULTIALLOC_DECL(&ma, struct vk_render_pass_attachment, attachments,
                           pCreateInfo->attachmentCount);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass *, subpasses,
                           pCreateInfo->subpassCount);
   VK_MULTIALLOC_DECL(&ma, struct vk_subpass_dependency, dependencies,
                           pCreateInfo->dependencyCount);

   if (!vk_object_multizalloc(device, &ma, pAllocator,
                              VK_OBJECT_TYPE_RENDER_PASS))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   pass->attachment_count = pCreateInfo->attachmentCount;
   pass->attachments = attachments;
   pass->subpass_count = pCreateInfo->subpassCount;
   pass->subpasses = subpasses;
   pass->dependency_count = pCreateInfo->dependencyCount;
   pass->dependencies = dependencies;

   for (uint32_t a = 0; a < pCreateInfo->attachmentCount; a++) {
      vk_render_pass_attachment_init(&pass->attachments[a],
                                     &pCreateInfo->pAttachments[a]);
   }

   for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
      VkResult result =
         vk_subpass_create(pCreateInfo, pAllocator, pass, i);
      if (result != VK_SUCCESS) {
         vk_common_DestroyRenderPass(_device,
                                     vk_render_pass_to_handle(pass),
                                     pAllocator);
         return result;
      }
   }

   /* Walk backwards over the subpasses to compute view masks and
    * last_subpass masks for all attachments.
    */
   for (uint32_t s = 0; s < pCreateInfo->subpassCount; s++) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(pass, pCreateInfo->subpassCount - 1 - s);
      struct vk_subpass_attachment_iter att_iter;

      /* First, compute last_subpass for all the attachments */
      vk_subpass_foreach_attachment(subpass, &att_iter, att) {
         if (att->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         assert(att->attachment < pass->attachment_count);
         const struct vk_render_pass_attachment *pass_att =
            &pass->attachments[att->attachment];

         att->last_subpass = subpass->view_mask & ~pass_att->view_mask;
      }

      /* Then compute pass_att->view_mask.  We do the two separately so that
       * we end up with the right last_subpass even if the same attachment is
       * used twice within a subpass.
       */
      vk_subpass_foreach_attachment(subpass, &att_iter, att) {
         if (att->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         assert(att->attachment < pass->attachment_count);
         struct vk_render_pass_attachment *pass_att =
            &pass->attachments[att->attachment];

         pass_att->view_mask |= subpass->view_mask;
      }
   }

   pass->dependency_count = pCreateInfo->dependencyCount;
   for (uint32_t d = 0; d < pCreateInfo->dependencyCount; d++) {
      const VkSubpassDependency2 *dep = &pCreateInfo->pDependencies[d];

      pass->dependencies[d] = (struct vk_subpass_dependency) {
         .flags = dep->dependencyFlags,
         .src_subpass = dep->srcSubpass,
         .dst_subpass = dep->dstSubpass,
         .src_stage_mask = (VkPipelineStageFlags2)dep->srcStageMask,
         .dst_stage_mask = (VkPipelineStageFlags2)dep->dstStageMask,
         .src_access_mask = (VkAccessFlags2)dep->srcAccessMask,
         .dst_access_mask = (VkAccessFlags2)dep->dstAccessMask,
         .view_offset = dep->viewOffset,
      };

      /* From the Vulkan 1.3.204 spec:
       *
       *    "If a VkMemoryBarrier2 is included in the pNext chain,
       *    srcStageMask, dstStageMask, srcAccessMask, and dstAccessMask
       *    parameters are ignored. The synchronization and access scopes
       *    instead are defined by the parameters of VkMemoryBarrier2."
       */
      const VkMemoryBarrier2 *barrier =
         vk_find_struct_const(dep->pNext, MEMORY_BARRIER_2);
      if (barrier != NULL) {
         pass->dependencies[d].src_stage_mask = barrier->srcStageMask;
         pass->dependencies[d].dst_stage_mask = barrier->dstStageMask;
         pass->dependencies[d].src_access_mask = barrier->srcAccessMask;
         pass->dependencies[d].dst_access_mask = barrier->dstAccessMask;
      }
   }

   const VkRenderPassFragmentDensityMapCreateInfoEXT *fdm_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT);
   if (fdm_info) {
      pass->fragment_density_map = fdm_info->fragmentDensityMapAttachment;
   } else {
      pass->fragment_density_map.attachment = VK_ATTACHMENT_UNUSED;
      pass->fragment_density_map.layout = VK_IMAGE_LAYOUT_UNDEFINED;
   }

   *pRenderPass = vk_render_pass_to_handle(pass);

   return VK_SUCCESS;
}

const VkPipelineRenderingCreateInfo *
vk_get_pipeline_rendering_create_info(const VkGraphicsPipelineCreateInfo *info)
{
   VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, info->subpass);
      return &subpass->pipeline_info;
   }

   return vk_find_struct_const(info->pNext, PIPELINE_RENDERING_CREATE_INFO);
}

const VkRenderingInputAttachmentIndexInfo *
vk_get_pipeline_rendering_ial_info(const VkGraphicsPipelineCreateInfo *info)
{
   VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, info->subpass);
      return &subpass->ial.info;
   }

   return vk_find_struct_const(info->pNext,
                               RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR);
}

const VkRenderingAttachmentLocationInfo *
vk_get_pipeline_rendering_cal_info(const VkGraphicsPipelineCreateInfo *info)
{
   VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, info->subpass);
      return &subpass->cal.info;
   }

   return vk_find_struct_const(info->pNext,
                               RENDERING_ATTACHMENT_LOCATION_INFO_KHR);
}

VkPipelineCreateFlags2KHR
vk_get_pipeline_rendering_flags(const VkGraphicsPipelineCreateInfo *info)
{
   VkPipelineCreateFlags2KHR rendering_flags = 0;

   VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, info->subpass);
      rendering_flags |= subpass->pipeline_flags;
      if (render_pass->fragment_density_map.attachment != VK_ATTACHMENT_UNUSED)
         rendering_flags |=
            VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
   }

   return rendering_flags;
}

const VkAttachmentSampleCountInfoAMD *
vk_get_pipeline_sample_count_info_amd(const VkGraphicsPipelineCreateInfo *info)
{
   VK_FROM_HANDLE(vk_render_pass, render_pass, info->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, info->subpass);

      return &subpass->sample_count_info_amd;
   }

   return vk_find_struct_const(info->pNext, ATTACHMENT_SAMPLE_COUNT_INFO_AMD);
}

const VkCommandBufferInheritanceRenderingInfo *
vk_get_command_buffer_inheritance_rendering_info(
   VkCommandBufferLevel level,
   const VkCommandBufferBeginInfo *pBeginInfo)
{
   /* From the Vulkan 1.3.204 spec:
    *
    *    "VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT specifies that a
    *    secondary command buffer is considered to be entirely inside a render
    *    pass. If this is a primary command buffer, then this bit is ignored."
    *
    * Since we're only concerned with the continue case here, we can ignore
    * any primary command buffers.
    */
   if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      return NULL;

   if (!(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))
      return NULL;

   const VkCommandBufferInheritanceInfo *inheritance =
      pBeginInfo->pInheritanceInfo;

   /* From the Vulkan 1.3.204 spec:
    *
    *    "If VkCommandBufferInheritanceInfo::renderPass is not VK_NULL_HANDLE,
    *    or VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT is not specified
    *    in VkCommandBufferBeginInfo::flags, parameters of this structure are
    *    ignored."
    *
    * If we have a render pass that wins, even if a
    * VkCommandBufferInheritanceRenderingInfo struct is included in the pNext
    * chain.
    */
   VK_FROM_HANDLE(vk_render_pass, render_pass, inheritance->renderPass);
   if (render_pass != NULL) {
      struct vk_subpass *subpass =
         vk_render_pass_get_subpass(render_pass, inheritance->subpass);
      return &subpass->inheritance_info;
   }

   return vk_find_struct_const(inheritance->pNext,
                               COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
}

const VkRenderingInfo *
vk_get_command_buffer_inheritance_as_rendering_resume(
   VkCommandBufferLevel level,
   const VkCommandBufferBeginInfo *pBeginInfo,
   void *stack_data)
{
   struct vk_gcbiarr_data *data = stack_data;

   /* From the Vulkan 1.3.204 spec:
    *
    *    "VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT specifies that a
    *    secondary command buffer is considered to be entirely inside a render
    *    pass. If this is a primary command buffer, then this bit is ignored."
    *
    * Since we're only concerned with the continue case here, we can ignore
    * any primary command buffers.
    */
   if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      return NULL;

   if (!(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))
      return NULL;

   const VkCommandBufferInheritanceInfo *inheritance =
      pBeginInfo->pInheritanceInfo;

   VK_FROM_HANDLE(vk_render_pass, pass, inheritance->renderPass);
   if (pass == NULL)
      return NULL;

   assert(inheritance->subpass < pass->subpass_count);
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, inheritance->subpass);

   VK_FROM_HANDLE(vk_framebuffer, fb, inheritance->framebuffer);
   if (fb == NULL || (fb->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
      return NULL;

   data->rendering = (VkRenderingInfo) {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_RESUMING_BIT,
      .renderArea = {
         .offset = { 0, 0 },
         .extent = { fb->width, fb->height },
      },
      .layerCount = fb->layers,
      .viewMask = pass->is_multiview ? subpass->view_mask : 0,
   };

   VkRenderingAttachmentInfo *attachments = data->attachments;

   for (unsigned i = 0; i < subpass->color_count; i++) {
      const struct vk_subpass_attachment *sp_att =
         &subpass->color_attachments[i];
      if (sp_att->attachment == VK_ATTACHMENT_UNUSED) {
         attachments[i] = (VkRenderingAttachmentInfo) {
            .imageView = VK_NULL_HANDLE,
         };
         continue;
      }

      assert(sp_att->attachment < pass->attachment_count);
      attachments[i] = (VkRenderingAttachmentInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = fb->attachments[sp_att->attachment],
         .imageLayout = sp_att->layout,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };
   }
   data->rendering.colorAttachmentCount = subpass->color_count;
   data->rendering.pColorAttachments = attachments;
   attachments += subpass->color_count;

   if (subpass->depth_stencil_attachment) {
      const struct vk_subpass_attachment *sp_att =
         subpass->depth_stencil_attachment;
      assert(sp_att->attachment < pass->attachment_count);

      VK_FROM_HANDLE(vk_image_view, iview, fb->attachments[sp_att->attachment]);
      if (iview->image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         *attachments = (VkRenderingAttachmentInfo) {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = vk_image_view_to_handle(iview),
            .imageLayout = sp_att->layout,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         };
         data->rendering.pDepthAttachment = attachments++;
      }

      if (iview->image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         *attachments = (VkRenderingAttachmentInfo) {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = vk_image_view_to_handle(iview),
            .imageLayout = sp_att->stencil_layout,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         };
         data->rendering.pStencilAttachment = attachments++;
      }
   }

   if (subpass->fragment_shading_rate_attachment) {
      const struct vk_subpass_attachment *sp_att =
         subpass->fragment_shading_rate_attachment;
      assert(sp_att->attachment < pass->attachment_count);

      data->fsr_att = (VkRenderingFragmentShadingRateAttachmentInfoKHR) {
         .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
         .imageView = fb->attachments[sp_att->attachment],
         .imageLayout = sp_att->layout,
         .shadingRateAttachmentTexelSize =
            subpass->fragment_shading_rate_attachment_texel_size,
      };
      __vk_append_struct(&data->rendering, &data->fsr_att);
   }

   /* Append this one last because it lives in the subpass and we don't want
    * to be changed by appending other structures later.
    */
   if (subpass->mrtss.multisampledRenderToSingleSampledEnable)
      __vk_append_struct(&data->rendering, (void *)&subpass->mrtss);

   return &data->rendering;
}

const VkRenderingAttachmentLocationInfoKHR *
vk_get_command_buffer_rendering_attachment_location_info(
   VkCommandBufferLevel level,
   const VkCommandBufferBeginInfo *pBeginInfo)
{
   /* From the Vulkan 1.3.295 spec:
    *
    *    "VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT specifies that a
    *    secondary command buffer is considered to be entirely inside a render
    *    pass. If this is a primary command buffer, then this bit is ignored."
    *
    * Since we're only concerned with the continue case here, we can ignore
    * any primary command buffers.
    */
   if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      return NULL;

   /* From the Vulkan 1.3.295 spec:
    *
    *    "This structure can be included in the pNext chain of a
    *    VkCommandBufferInheritanceInfo structure to specify inherited state
    *    from the primary command buffer. If
    *    VkCommandBufferInheritanceInfo::renderPass is not VK_NULL_HANDLE, or
    *    VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT is not specified in
    *    VkCommandBufferBeginInfo::flags, members of this structure are
    *    ignored."
    *
    * For the case where a render pass is provided and we're emulating it on
    * behalf of the driver, the default NULL behavior is sufficient:
    *
    *    "If this structure is not included in the pNext chain of
    *    VkCommandBufferInheritanceInfo, it is equivalent to specifying this
    *    structure with the following properties:
    *
    *     - colorAttachmentCount set to
    *       VkCommandBufferInheritanceRenderingInfo::colorAttachmentCount.
    *
    *     - pColorAttachmentLocations set to NULL."
    */
   if (!(pBeginInfo->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))
      return NULL;

   if (pBeginInfo->pInheritanceInfo->renderPass != VK_NULL_HANDLE)
      return NULL;

   return vk_find_struct_const(pBeginInfo,
                               RENDERING_ATTACHMENT_LOCATION_INFO_KHR);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_DestroyRenderPass(VkDevice _device,
                            VkRenderPass renderPass,
                            const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VK_FROM_HANDLE(vk_render_pass, pass, renderPass);

   if (!pass)
      return;

   for (uint32_t i = 0; i < pass->subpass_count; i++) {
      struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, i);
      if (subpass->merged == VK_SUBPASS_MERGED_FIRST ||
          subpass->merged == VK_SUBPASS_NOT_MERGED)
         vk_free2(&device->alloc, pAllocator, subpass);
   }

   vk_object_free(device, pAllocator, pass);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetRenderAreaGranularity(VkDevice device,
                                   VkRenderPass renderPass,
                                   VkExtent2D *pGranularity)
{
   *pGranularity = (VkExtent2D){1, 1};
}

VKAPI_ATTR void VKAPI_CALL
vk_common_GetRenderingAreaGranularityKHR(
   VkDevice _device, const VkRenderingAreaInfoKHR *pRenderingAreaInfo,
   VkExtent2D *pGranularity)
{
   *pGranularity = (VkExtent2D) { 1, 1 };
}

static VkRenderPassSampleLocationsBeginInfoEXT *
clone_rp_sample_locations(const VkRenderPassSampleLocationsBeginInfoEXT *loc)
{
   uint32_t sl_count = 0;

   for (uint32_t i = 0; i < loc->attachmentInitialSampleLocationsCount; i++) {
      const VkAttachmentSampleLocationsEXT *att_sl_in =
         &loc->pAttachmentInitialSampleLocations[i];
      sl_count += att_sl_in->sampleLocationsInfo.sampleLocationsCount;
   }
   for (uint32_t i = 0; i < loc->postSubpassSampleLocationsCount; i++) {
      const VkSubpassSampleLocationsEXT *sp_sl_in =
         &loc->pPostSubpassSampleLocations[i];
      sl_count += sp_sl_in->sampleLocationsInfo.sampleLocationsCount;
   }

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, VkRenderPassSampleLocationsBeginInfoEXT, new_loc, 1);
   VK_MULTIALLOC_DECL(&ma, VkAttachmentSampleLocationsEXT, new_att_sl,
                      loc->attachmentInitialSampleLocationsCount);
   VK_MULTIALLOC_DECL(&ma, VkSubpassSampleLocationsEXT, new_sp_sl,
                      loc->postSubpassSampleLocationsCount);
   VK_MULTIALLOC_DECL(&ma, VkSampleLocationEXT, sl, sl_count);
   if (!vk_multialloc_alloc(&ma, vk_default_allocator(),
                            VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return NULL;

   VkSampleLocationEXT *next_sl = sl;
   for (uint32_t i = 0; i < loc->attachmentInitialSampleLocationsCount; i++) {
      const VkAttachmentSampleLocationsEXT *att_sl_in =
         &loc->pAttachmentInitialSampleLocations[i];
      const VkSampleLocationsInfoEXT *sli_in = &att_sl_in->sampleLocationsInfo;

      typed_memcpy(next_sl, sli_in->pSampleLocations,
                   sli_in->sampleLocationsCount);

      new_att_sl[i] = (VkAttachmentSampleLocationsEXT) {
         .attachmentIndex = att_sl_in->attachmentIndex,
         .sampleLocationsInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = sli_in->sampleLocationsPerPixel,
            .sampleLocationGridSize = sli_in->sampleLocationGridSize,
            .sampleLocationsCount = sli_in->sampleLocationsCount,
            .pSampleLocations = next_sl,
         },
      };

      next_sl += sli_in->sampleLocationsCount;
   }

   for (uint32_t i = 0; i < loc->postSubpassSampleLocationsCount; i++) {
      const VkSubpassSampleLocationsEXT *sp_sl_in =
         &loc->pPostSubpassSampleLocations[i];
      const VkSampleLocationsInfoEXT *sli_in = &sp_sl_in->sampleLocationsInfo;

      typed_memcpy(next_sl, sli_in->pSampleLocations,
                   sli_in->sampleLocationsCount);

      new_sp_sl[i] = (VkSubpassSampleLocationsEXT) {
         .subpassIndex = sp_sl_in->subpassIndex,
         .sampleLocationsInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = sli_in->sampleLocationsPerPixel,
            .sampleLocationGridSize = sli_in->sampleLocationGridSize,
            .sampleLocationsCount = sli_in->sampleLocationsCount,
            .pSampleLocations = next_sl,
         },
      };

      next_sl += sli_in->sampleLocationsCount;
   }

   assert(next_sl == sl + sl_count);

   *new_loc = (VkRenderPassSampleLocationsBeginInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,
      .attachmentInitialSampleLocationsCount = loc->attachmentInitialSampleLocationsCount,
      .pAttachmentInitialSampleLocations = new_att_sl,
      .postSubpassSampleLocationsCount = loc->postSubpassSampleLocationsCount,
      .pPostSubpassSampleLocations = new_sp_sl,
   };

   return new_loc;
}

static const VkSampleLocationsInfoEXT *
get_subpass_sample_locations(const VkRenderPassSampleLocationsBeginInfoEXT *loc,
                             uint32_t subpass_idx)
{
   for (uint32_t i = 0; i < loc->postSubpassSampleLocationsCount; i++) {
      if (loc->pPostSubpassSampleLocations[i].subpassIndex == subpass_idx)
         return &loc->pPostSubpassSampleLocations[i].sampleLocationsInfo;
   }

   return NULL;
}

static bool
vk_image_layout_supports_input_attachment(VkImageLayout layout)
{
   switch (layout) {
   case VK_IMAGE_LAYOUT_GENERAL:
   case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
   case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
   case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
      return true;
   default:
      return false;
   }
}

struct stage_access {
   VkPipelineStageFlagBits2 stages;
   VkAccessFlagBits2 access;
};

static bool
vk_image_layout_are_all_aspects_read_only(VkImageLayout layout,
                                          VkImageAspectFlags aspects)
{
   u_foreach_bit(a, aspects) {
      VkImageAspectFlagBits aspect = 1u << a;
      if (!vk_image_layout_is_read_only(layout, aspect))
         return false;
   }
   return true;
}

static struct stage_access
stage_access_for_layout(VkImageLayout layout, VkImageAspectFlags aspects)
{
   VkPipelineStageFlagBits2 stages = 0;
   VkAccessFlagBits2 access = 0;

   if (vk_image_layout_supports_input_attachment(layout)) {
      stages |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      access |= VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
   }

   if (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
      stages |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
      if (!vk_image_layout_are_all_aspects_read_only(layout, aspects)) {
         access |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

         /* It might be a resolve attachment */
         stages |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
         access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
      }
   } else {
      /* Color */
      if (!vk_image_layout_are_all_aspects_read_only(layout, aspects)) {
         /* There are no read-only color attachments */
         stages |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
         access |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

         /* It might be a resolve attachment */
         stages |= VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
         access |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
      }
   }

   return (struct stage_access) {
      .stages = stages,
      .access = access,
   };
}

static void
transition_image_range(const struct vk_image_view *image_view,
                       VkImageSubresourceRange range,
                       VkImageLayout old_layout,
                       VkImageLayout new_layout,
                       VkImageLayout old_stencil_layout,
                       VkImageLayout new_stencil_layout,
                       const VkSampleLocationsInfoEXT *sample_locations,
                       uint32_t *barrier_count,
                       uint32_t max_barrier_count,
                       VkImageMemoryBarrier2 *barriers)
{
   VkImageAspectFlags aspects_left = range.aspectMask;
   while (aspects_left) {
      range.aspectMask = aspects_left;

      /* If we have a depth/stencil image and one of the layouts doesn't match
       * between depth and stencil, we need two barriers.  Restrict to depth
       * and we'll pick up stencil on the next iteration.
       */
      if (range.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT |
                               VK_IMAGE_ASPECT_STENCIL_BIT) &&
          (old_layout != old_stencil_layout ||
           new_layout != new_stencil_layout))
         range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

      if (range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) {
         /* We're down to a single aspect bit so this is going to be the last
          * iteration and it's fine to stomp the input variables here.
          */
         old_layout = old_stencil_layout;
         new_layout = new_stencil_layout;
      }

      if (new_layout != old_layout) {
         /* We could go about carefully calculating every possible way the
          * attachment may have been used in the render pass or we can break
          * out the big hammer and throw in any stage and access flags
          * possible for the given layouts.
          */
         struct stage_access src_sa, dst_sa;
         src_sa = stage_access_for_layout(old_layout, range.aspectMask);
         dst_sa = stage_access_for_layout(new_layout, range.aspectMask);

         assert(*barrier_count < max_barrier_count);
         barriers[(*barrier_count)++] = (VkImageMemoryBarrier2) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = sample_locations,
            .srcStageMask = src_sa.stages,
            .srcAccessMask = src_sa.access,
            .dstStageMask = dst_sa.stages,
            .dstAccessMask = dst_sa.access,
            .oldLayout = old_layout,
            .newLayout = new_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk_image_to_handle(image_view->image),
            .subresourceRange = range,
         };
      }

      aspects_left &= ~range.aspectMask;
   }
}

static bool
can_use_attachment_initial_layout(struct vk_command_buffer *cmd_buffer,
                                  uint32_t att_idx,
                                  uint32_t view_mask,
                                  VkImageLayout *layout_out,
                                  VkImageLayout *stencil_layout_out)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_framebuffer *framebuffer = cmd_buffer->framebuffer;
   const struct vk_render_pass_attachment *rp_att = &pass->attachments[att_idx];
   struct vk_attachment_state *att_state = &cmd_buffer->attachments[att_idx];
   const struct vk_image_view *image_view = att_state->image_view;

   if ((rp_att->aspects & ~VK_IMAGE_ASPECT_STENCIL_BIT) &&
       rp_att->load_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
      return false;

   if ((rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       rp_att->stencil_load_op != VK_ATTACHMENT_LOAD_OP_CLEAR)
      return false;

   if (cmd_buffer->render_area.offset.x != 0 ||
       cmd_buffer->render_area.offset.y != 0 ||
       cmd_buffer->render_area.extent.width != image_view->extent.width ||
       cmd_buffer->render_area.extent.height != image_view->extent.height)
      return false;

   if (image_view->image->image_type == VK_IMAGE_TYPE_3D) {
      /* For 3D images, the view has to be the whole thing */
      if (image_view->base_array_layer != 0)
         return false;

      if (pass->is_multiview) {
         if (!util_is_power_of_two_or_zero(view_mask + 1) ||
             util_last_bit(view_mask) != image_view->layer_count)
            return false;
      } else {
         if (framebuffer->layers != image_view->layer_count)
            return false;
      }
   }

   /* Finally, check if the entire thing is undefined.  It's ok to smash the
    * view_mask now as the only thing using it will be the loop below.
    */

   /* 3D is stupidly special.  See transition_attachment() */
   if (image_view->image->image_type == VK_IMAGE_TYPE_3D)
      view_mask = 1;

   VkImageLayout layout = VK_IMAGE_LAYOUT_MAX_ENUM;
   VkImageLayout stencil_layout = VK_IMAGE_LAYOUT_MAX_ENUM;

   assert(view_mask != 0);
   u_foreach_bit(view, view_mask) {
      assert(view >= 0 && view < MESA_VK_MAX_MULTIVIEW_VIEW_COUNT);
      struct vk_attachment_view_state *att_view_state = &att_state->views[view];

      if (rp_att->aspects & ~VK_IMAGE_ASPECT_STENCIL_BIT) {
         if (layout == VK_IMAGE_LAYOUT_MAX_ENUM)
            layout = att_view_state->layout;
         else if (layout != att_view_state->layout)
            return false;
      }

      if (rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         if (stencil_layout == VK_IMAGE_LAYOUT_MAX_ENUM)
            stencil_layout = att_view_state->stencil_layout;
         else if (stencil_layout != att_view_state->stencil_layout)
            return false;
      }
   }

   if (layout != VK_IMAGE_LAYOUT_MAX_ENUM)
      *layout_out = layout;
   else if (layout_out != NULL)
      *layout_out = VK_IMAGE_LAYOUT_UNDEFINED;

   if (stencil_layout != VK_IMAGE_LAYOUT_MAX_ENUM)
      *stencil_layout_out = stencil_layout;
   else if (stencil_layout_out != NULL)
      *stencil_layout_out = VK_IMAGE_LAYOUT_UNDEFINED;

   return true;
}

uint32_t
vk_command_buffer_get_attachment_layout(const struct vk_command_buffer *cmd_buffer,
                                        const struct vk_image *image,
                                        VkImageLayout *out_layout,
                                        VkImageLayout *out_stencil_layout)
{
   const struct vk_render_pass *render_pass = cmd_buffer->render_pass;
   assert(render_pass != NULL);

   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(render_pass, cmd_buffer->subpass_idx);
   int first_view = ffs(subpass->view_mask) - 1;

   for (uint32_t a = 0; a < render_pass->attachment_count; a++) {
      if (cmd_buffer->attachments[a].image_view->image == image) {
         *out_layout = cmd_buffer->attachments[a].views[first_view].layout;
         *out_stencil_layout =
            cmd_buffer->attachments[a].views[first_view].stencil_layout;
         return a;
      }
   }
   unreachable("Image not found in attachments");
}

void
vk_command_buffer_set_attachment_layout(struct vk_command_buffer *cmd_buffer,
                                        uint32_t att_idx,
                                        VkImageLayout layout,
                                        VkImageLayout stencil_layout)
{
   const struct vk_render_pass *render_pass = cmd_buffer->render_pass;
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(render_pass, cmd_buffer->subpass_idx);
   uint32_t view_mask = subpass->view_mask;
   struct vk_attachment_state *att_state = &cmd_buffer->attachments[att_idx];

   u_foreach_bit(view, view_mask) {
      assert(view >= 0 && view < MESA_VK_MAX_MULTIVIEW_VIEW_COUNT);
      struct vk_attachment_view_state *att_view_state = &att_state->views[view];

      att_view_state->layout = layout;
      att_view_state->stencil_layout = stencil_layout;
   }
}

static void
transition_attachment(struct vk_command_buffer *cmd_buffer,
                      uint32_t att_idx,
                      uint32_t view_mask,
                      VkImageLayout layout,
                      VkImageLayout stencil_layout,
                      uint32_t *barrier_count,
                      uint32_t max_barrier_count,
                      VkImageMemoryBarrier2 *barriers)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_framebuffer *framebuffer = cmd_buffer->framebuffer;
   const struct vk_render_pass_attachment *pass_att =
      &pass->attachments[att_idx];
   struct vk_attachment_state *att_state = &cmd_buffer->attachments[att_idx];
   const struct vk_image_view *image_view = att_state->image_view;

   /* 3D is stupidly special.  From the Vulkan 1.3.204 spec:
    *
    *    "When the VkImageSubresourceRange structure is used to select a
    *    subset of the slices of a 3D image’s mip level in order to create
    *    a 2D or 2D array image view of a 3D image created with
    *    VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT, baseArrayLayer and
    *    layerCount specify the first slice index and the number of slices
    *    to include in the created image view. Such an image view can be
    *    used as a framebuffer attachment that refers only to the specified
    *    range of slices of the selected mip level. However, any layout
    *    transitions performed on such an attachment view during a render
    *    pass instance still apply to the entire subresource referenced
    *    which includes all the slices of the selected mip level."
    *
    * To deal with this, we expand out the layer range to include the
    * entire 3D image and treat them as having only a single view even when
    * multiview is enabled.  This later part means that we effectively only
    * track one image layout for the entire attachment rather than one per
    * view like we do for all the others.
    */
   if (image_view->image->image_type == VK_IMAGE_TYPE_3D)
      view_mask = 1;

   u_foreach_bit(view, view_mask) {
      assert(view >= 0 && view < MESA_VK_MAX_MULTIVIEW_VIEW_COUNT);
      struct vk_attachment_view_state *att_view_state = &att_state->views[view];

      /* First, check to see if we even need a transition */
      if (att_view_state->layout == layout &&
          att_view_state->stencil_layout == stencil_layout)
         continue;

      VkImageSubresourceRange range = {
         .aspectMask = pass_att->aspects,
         .baseMipLevel = image_view->base_mip_level,
         .levelCount = 1,
      };

      /* From the Vulkan 1.3.207 spec:
       *
       *    "Automatic layout transitions apply to the entire image
       *    subresource attached to the framebuffer. If multiview is not
       *    enabled and the attachment is a view of a 1D or 2D image, the
       *    automatic layout transitions apply to the number of layers
       *    specified by VkFramebufferCreateInfo::layers. If multiview is
       *    enabled and the attachment is a view of a 1D or 2D image, the
       *    automatic layout transitions apply to the layers corresponding to
       *    views which are used by some subpass in the render pass, even if
       *    that subpass does not reference the given attachment. If the
       *    attachment view is a 2D or 2D array view of a 3D image, even if
       *    the attachment view only refers to a subset of the slices of the
       *    selected mip level of the 3D image, automatic layout transitions
       *    apply to the entire subresource referenced which is the entire mip
       *    level in this case."
       */
      if (image_view->image->image_type == VK_IMAGE_TYPE_3D) {
         assert(view == 0);
         range.baseArrayLayer = 0;
         range.layerCount = image_view->extent.depth;
      } else if (pass->is_multiview) {
         range.baseArrayLayer = image_view->base_array_layer + view;
         range.layerCount = 1;
      } else {
         assert(view == 0);
         range.baseArrayLayer = image_view->base_array_layer;
         range.layerCount = framebuffer->layers;
      }

      transition_image_range(image_view, range,
                             att_view_state->layout, layout,
                             att_view_state->stencil_layout, stencil_layout,
                             att_view_state->sample_locations,
                             barrier_count, max_barrier_count, barriers);

      att_view_state->layout = layout;
      att_view_state->stencil_layout = stencil_layout;
   }
}

static void
load_attachment(struct vk_command_buffer *cmd_buffer,
                uint32_t att_idx, uint32_t view_mask,
                VkImageLayout layout, VkImageLayout stencil_layout)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_framebuffer *framebuffer = cmd_buffer->framebuffer;
   const struct vk_render_pass_attachment *rp_att = &pass->attachments[att_idx];
   struct vk_attachment_state *att_state = &cmd_buffer->attachments[att_idx];
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;

   printf("%s:%i att %d %p load_op=%d view_mask %x views_loaded %x\n", __func__, __LINE__, att_idx, rp_att, rp_att->load_op, view_mask, att_state->views_loaded);
   /* Don't load any views we've already loaded */
   view_mask &= ~att_state->views_loaded;
   if (view_mask == 0)
      return;

   printf("%s:%i att %d\n", __func__, __LINE__, att_idx);
   /* From here on, if we return, we loaded the views */
   att_state->views_loaded |= view_mask;

   /* We only need to load/store if there's a clear */
   bool need_load_store = false;
   if ((rp_att->aspects & ~VK_IMAGE_ASPECT_STENCIL_BIT) &&
       rp_att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      need_load_store = true;

   if ((rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
       rp_att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR)
      need_load_store = true;

   if (!need_load_store)
      return;

   const VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = vk_image_view_to_handle(att_state->image_view),
      .imageLayout = layout,
      .loadOp = rp_att->load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = att_state->clear_value,
   };

   const VkRenderingAttachmentInfo stencil_att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = vk_image_view_to_handle(att_state->image_view),
      .imageLayout = stencil_layout,
      .loadOp = rp_att->stencil_load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = att_state->clear_value,
   };

   VkRenderingInfo render = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_INPUT_ATTACHMENT_NO_CONCURRENT_WRITES_BIT_MESA,
      .renderArea = cmd_buffer->render_area,
      .layerCount = pass->is_multiview ? 1 : framebuffer->layers,
      .viewMask = pass->is_multiview ? view_mask : 0,
   };

   if (rp_att->aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                          VK_IMAGE_ASPECT_STENCIL_BIT)) {
      if (rp_att->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
         render.pDepthAttachment = &att;
      if (rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
         render.pStencilAttachment = &stencil_att;
   } else {
      render.colorAttachmentCount = 1;
      render.pColorAttachments = &att;
   }

   disp->CmdBeginRendering(vk_command_buffer_to_handle(cmd_buffer), &render);
   disp->CmdEndRendering(vk_command_buffer_to_handle(cmd_buffer));
}

static void
subpass_prepare_color_attachments(
   struct vk_command_buffer *cmd_buffer,
   VkRenderingInfo *rendering,
   VkRenderingAttachmentInfo *color_attachments,
   VkRenderingAttachmentInitialLayoutInfoMESA *color_attachment_initial_layouts)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, cmd_buffer->subpass_idx);

   rendering->colorAttachmentCount = subpass->color_count;
   rendering->pColorAttachments = color_attachments;

   for (uint32_t i = 0; i < subpass->color_count; i++) {
      const struct vk_subpass_attachment *sp_att =
         &subpass->color_attachments[i];
      VkRenderingAttachmentInfo *color_attachment = &color_attachments[i];

      if (sp_att->attachment == VK_ATTACHMENT_UNUSED) {
         *color_attachment = (VkRenderingAttachmentInfo) {
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = VK_NULL_HANDLE,
         };
         continue;
      }

      assert(sp_att->attachment < pass->attachment_count);
      const struct vk_render_pass_attachment *rp_att =
         &pass->attachments[sp_att->attachment];
      struct vk_attachment_state *att_state =
         &cmd_buffer->attachments[sp_att->attachment];

      *color_attachment = (VkRenderingAttachmentInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = vk_image_view_to_handle(att_state->image_view),
         .imageLayout = sp_att->layout,
      };

      if (!(subpass->view_mask & att_state->views_loaded)) {
         /* None of these views have been used before */
         color_attachment->loadOp = rp_att->load_op;
         color_attachment->clearValue = att_state->clear_value;
         att_state->views_loaded |= subpass->view_mask;
         printf("%s:%i att %d att_state->views_loaded %x att_state->clear_value %x %x %x %x\n", __func__, __LINE__, sp_att->attachment, att_state->views_loaded, att_state->clear_value.color.uint32[0], att_state->clear_value.color.uint32[1], att_state->clear_value.color.uint32[2], att_state->clear_value.color.uint32[3]);

         VkImageLayout initial_layout;
         if (can_use_attachment_initial_layout(cmd_buffer,
                                               sp_att->attachment,
                                               subpass->view_mask,
                                               &initial_layout, NULL) &&
             sp_att->layout != initial_layout) {
            assert(color_attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);

            VkRenderingAttachmentInitialLayoutInfoMESA *color_initial_layout =
               &color_attachment_initial_layouts[i];
            *color_initial_layout = (VkRenderingAttachmentInitialLayoutInfoMESA) {
               .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA,
               .initialLayout = initial_layout,
            };
            __vk_append_struct(color_attachment, color_initial_layout);

            vk_command_buffer_set_attachment_layout(cmd_buffer,
                                                    sp_att->attachment,
                                                    sp_att->layout,
                                                    VK_IMAGE_LAYOUT_UNDEFINED);
         }
      } else {
         /* We've seen at least one of the views of this attachment before so
          * we need to LOAD_OP_LOAD.
          */
         color_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      }

      if (!(subpass->view_mask & ~sp_att->last_subpass)) {
         /* This is the last subpass for every view */
         color_attachment->storeOp = rp_att->store_op;
      } else {
         /* For at least one of our views, this isn't the last subpass
          *
          * In the edge case where we have lots of weird overlap between view
          * masks of different subThis may mean that we get STORE_OP_STORE in
          * some places where it may have wanted STORE_OP_NONE but that should
          * be harmless.
          */
         color_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }

      if (sp_att->resolve != NULL) {
         assert(sp_att->resolve->attachment < pass->attachment_count);
         struct vk_attachment_state *res_att_state =
            &cmd_buffer->attachments[sp_att->resolve->attachment];

         /* Resolve attachments are entirely overwritten by the resolve
          * operation so the load op really doesn't matter.  We can consider
          * the resolve as being the load.
          */
         res_att_state->views_loaded |= subpass->view_mask;

         if (vk_format_is_int(res_att_state->image_view->format))
            color_attachment->resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
         else
            color_attachment->resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;

         color_attachment->resolveImageView =
            vk_image_view_to_handle(res_att_state->image_view);
         color_attachment->resolveImageLayout = sp_att->resolve->layout;
      } else if (subpass->mrtss.multisampledRenderToSingleSampledEnable &&
                 rp_att->samples == VK_SAMPLE_COUNT_1_BIT) {
         if (vk_format_is_int(att_state->image_view->format))
            color_attachment->resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
         else
            color_attachment->resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      }
   }
}

static void
subpass_prepare_ds_attachments(
   struct vk_command_buffer *cmd_buffer,
   VkRenderingInfo *rendering,
   VkRenderingAttachmentInfo *depth_attachment,
   VkRenderingAttachmentInfo *stencil_attachment,
   VkRenderingAttachmentInitialLayoutInfoMESA *depth_initial_layout,
   VkRenderingAttachmentInitialLayoutInfoMESA *stencil_initial_layout,
   VkSampleLocationsInfoEXT *new_sample_locations)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const uint32_t subpass_idx = cmd_buffer->subpass_idx;
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, subpass_idx);

   *depth_attachment = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
   };
   *stencil_attachment = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
   };
   *depth_initial_layout = (VkRenderingAttachmentInitialLayoutInfoMESA){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA,
   };
   *stencil_initial_layout = (VkRenderingAttachmentInitialLayoutInfoMESA){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INITIAL_LAYOUT_INFO_MESA,
   };

   rendering->pDepthAttachment = depth_attachment;
   rendering->pStencilAttachment = stencil_attachment;

   const VkSampleLocationsInfoEXT *sample_locations = NULL;
   if (subpass->depth_stencil_attachment != NULL) {
      const struct vk_subpass_attachment *sp_att =
         subpass->depth_stencil_attachment;

      assert(sp_att->attachment < pass->attachment_count);
      const struct vk_render_pass_attachment *rp_att =
         &pass->attachments[sp_att->attachment];
      struct vk_attachment_state *att_state =
         &cmd_buffer->attachments[sp_att->attachment];

      assert(sp_att->aspects == rp_att->aspects);
      if (rp_att->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         depth_attachment->imageView =
            vk_image_view_to_handle(att_state->image_view);
         depth_attachment->imageLayout = sp_att->layout;
      }

      if (rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         stencil_attachment->imageView =
            vk_image_view_to_handle(att_state->image_view);
         stencil_attachment->imageLayout = sp_att->stencil_layout;
      }

      if (!(subpass->view_mask & att_state->views_loaded)) {
         /* None of these views have been used before */
         depth_attachment->loadOp = rp_att->load_op;
         depth_attachment->clearValue = att_state->clear_value;
         stencil_attachment->loadOp = rp_att->stencil_load_op;
         stencil_attachment->clearValue = att_state->clear_value;
         att_state->views_loaded |= subpass->view_mask;

         VkImageLayout initial_layout, initial_stencil_layout;
         if (can_use_attachment_initial_layout(cmd_buffer,
                                               sp_att->attachment,
                                               subpass->view_mask,
                                               &initial_layout,
                                               &initial_stencil_layout)) {
            if ((rp_att->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
                sp_att->layout != initial_layout) {
               assert(depth_attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
               depth_initial_layout->initialLayout = initial_layout;
               __vk_append_struct(depth_attachment,
                                  depth_initial_layout);
            }

            if ((rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                sp_att->stencil_layout != initial_stencil_layout) {
               assert(stencil_attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
               stencil_initial_layout->initialLayout = initial_stencil_layout;
               __vk_append_struct(stencil_attachment,
                                  stencil_initial_layout);
            }

            vk_command_buffer_set_attachment_layout(cmd_buffer,
                                                    sp_att->attachment,
                                                    sp_att->layout,
                                                    sp_att->stencil_layout);
         }
      } else {
         /* We've seen at least one of the views of this attachment before so
          * we need to LOAD_OP_LOAD.
          */
         depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
         stencil_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      }

      if (!(subpass->view_mask & ~sp_att->last_subpass)) {
         /* This is the last subpass for every view */
         depth_attachment->storeOp = rp_att->store_op;
         stencil_attachment->storeOp = rp_att->stencil_store_op;
      } else {
         /* For at least one of our views, this isn't the last subpass
          *
          * In the edge case where we have lots of weird overlap between view
          * masks of different subThis may mean that we get STORE_OP_STORE in
          * some places where it may have wanted STORE_OP_NONE but that should
          * be harmless.
          */
         depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
         stencil_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      }

      /* From the Vulkan 1.3.212 spec:
       *
       *    "If the current render pass does not use the attachment as a
       *    depth/stencil attachment in any subpass that happens-before, the
       *    automatic layout transition uses the sample locations state
       *    specified in the sampleLocationsInfo member of the element of the
       *    VkRenderPassSampleLocationsBeginInfoEXT::pAttachmentInitialSampleLocations
       *    array for which the attachmentIndex member equals the attachment
       *    index of the attachment, if one is specified. Otherwise, the
       *    automatic layout transition uses the sample locations state
       *    specified in the sampleLocationsInfo member of the element of the
       *    VkRenderPassSampleLocationsBeginInfoEXT::pPostSubpassSampleLocations
       *    array for which the subpassIndex member equals the index of the
       *    subpass that last used the attachment as a depth/stencil
       *    attachment, if one is specified."
       *
       * Unfortunately, this says nothing whatsoever about multiview.
       * However, since multiview render passes are described as a single-view
       * render pass repeated per-view, we assume this is per-view.
       */
      if (cmd_buffer->pass_sample_locations != NULL &&
          (att_state->image_view->image->create_flags &
           VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT)) {
         sample_locations =
            get_subpass_sample_locations(cmd_buffer->pass_sample_locations,
                                         subpass_idx);

         u_foreach_bit(view, subpass->view_mask)
            att_state->views[view].sample_locations = sample_locations;
      }

      if (sp_att->resolve != NULL ||
          (subpass->mrtss.multisampledRenderToSingleSampledEnable &&
           rp_att->samples == VK_SAMPLE_COUNT_1_BIT)) {
         const struct vk_subpass_attachment *res_sp_att = sp_att->resolve ? sp_att->resolve : sp_att;
         assert(res_sp_att->attachment < pass->attachment_count);
         const struct vk_render_pass_attachment *res_rp_att =
            &pass->attachments[res_sp_att->attachment];
         struct vk_attachment_state *res_att_state =
            &cmd_buffer->attachments[res_sp_att->attachment];

         /* From the Vulkan 1.3.204 spec:
          *
          *    "VkSubpassDescriptionDepthStencilResolve::depthResolveMode is
          *    ignored if the VkFormat of the pDepthStencilResolveAttachment
          *    does not have a depth component. Similarly,
          *    VkSubpassDescriptionDepthStencilResolve::stencilResolveMode is
          *    ignored if the VkFormat of the pDepthStencilResolveAttachment
          *    does not have a stencil component."
          *
          * TODO: Should we handle this here or when we create the render
          * pass?  Handling it here makes load ops "correct" in the sense
          * that, if we resolve to the wrong aspect, we will still consider
          * it bound and clear it if requested.
          */
         VkResolveModeFlagBits depth_resolve_mode = VK_RESOLVE_MODE_NONE;
         if (res_rp_att->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
            depth_resolve_mode = subpass->depth_resolve_mode;

         VkResolveModeFlagBits stencil_resolve_mode = VK_RESOLVE_MODE_NONE;
         if (res_rp_att->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
            stencil_resolve_mode = subpass->stencil_resolve_mode;

         VkImageAspectFlags resolved_aspects = 0;

         if (depth_resolve_mode != VK_RESOLVE_MODE_NONE) {
            depth_attachment->resolveMode = depth_resolve_mode;
            if (sp_att->resolve) {
               depth_attachment->resolveImageView =
                  vk_image_view_to_handle(res_att_state->image_view);
               depth_attachment->resolveImageLayout =
                  sp_att->resolve->layout;
            }

            resolved_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
         }

         if (stencil_resolve_mode != VK_RESOLVE_MODE_NONE) {
            stencil_attachment->resolveMode = stencil_resolve_mode;
            if (sp_att->resolve) {
               stencil_attachment->resolveImageView =
                  vk_image_view_to_handle(res_att_state->image_view);
               stencil_attachment->resolveImageLayout =
                  sp_att->resolve->stencil_layout;
            }

            resolved_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
         }

         if (sp_att->resolve && resolved_aspects == rp_att->aspects) {
            /* The resolve attachment is entirely overwritten by the
             * resolve operation so the load op really doesn't matter.
             * We can consider the resolve as being the load.
             */
            res_att_state->views_loaded |= subpass->view_mask;
         }
      }
   }

   if (sample_locations) {
      *new_sample_locations = *sample_locations;
      __vk_append_struct(rendering, new_sample_locations);
   }
}

static void
begin_subpass_fb_local_barrier(struct vk_command_buffer *cmd_buffer)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const uint32_t subpass_idx = cmd_buffer->subpass_idx;
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;
   bool needs_mem_barrier = false;
   VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
   };

   for (uint32_t d = 0; d < pass->dependency_count; d++) {
      const struct vk_subpass_dependency *dep = &pass->dependencies[d];
      if (dep->dst_subpass != subpass_idx)
         continue;

      if (!(dep->flags & VK_DEPENDENCY_BY_REGION_BIT))
         continue;

      needs_mem_barrier = true;
      mem_barrier.srcStageMask |= dep->src_stage_mask;
      mem_barrier.srcAccessMask |= dep->src_access_mask;
      mem_barrier.dstStageMask |= dep->dst_stage_mask;
      mem_barrier.dstAccessMask |= dep->dst_access_mask;
   }

   if (needs_mem_barrier) {
      const VkDependencyInfo dependency_info = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
         .memoryBarrierCount = 1,
         .pMemoryBarriers = &mem_barrier,
      };
      cmd_buffer->runtime_rp_barrier = true;
      disp->CmdPipelineBarrier2(vk_command_buffer_to_handle(cmd_buffer),
                                &dependency_info);
      cmd_buffer->runtime_rp_barrier = false;
   }
}

static void
begin_subpass_barriers(struct vk_command_buffer *cmd_buffer)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const uint32_t subpass_idx = cmd_buffer->subpass_idx;
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;
   uint32_t first_subpass = subpass_idx, last_subpass = subpass_idx;
   for (; last_subpass < pass->subpass_count; last_subpass++) {
      struct vk_subpass *future_subpass =
         vk_render_pass_get_subpass(pass, last_subpass);

      if (future_subpass->merged == VK_SUBPASS_MERGED_LAST ||
          future_subpass->merged == VK_SUBPASS_NOT_MERGED)
         break;
   }

   assert(last_subpass < pass->subpass_count);

   bool needs_mem_barrier = false;
   VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
   };

   for (uint32_t d = 0; d < pass->dependency_count; d++) {
      const struct vk_subpass_dependency *dep = &pass->dependencies[d];
      if (dep->dst_subpass < first_subpass ||
          dep->dst_subpass > last_subpass)
         continue;

      /* Skip FB-local deps, those are handled with a pipeline barrier
       * inside the dynamic render pass. */
      if (dep->src_subpass > first_subpass &&
          dep->dst_subpass < last_subpass) {
         assert(dep->flags & VK_DEPENDENCY_BY_REGION_BIT);
         continue;
      }

      if (dep->flags & VK_DEPENDENCY_VIEW_LOCAL_BIT) {
         /* From the Vulkan 1.3.204 spec:
          *
          *    VUID-VkSubpassDependency2-dependencyFlags-03091
          *
          *    "If dependencyFlags includes VK_DEPENDENCY_VIEW_LOCAL_BIT,
          *    dstSubpass must not be equal to VK_SUBPASS_EXTERNAL"
          */
         assert(dep->src_subpass != VK_SUBPASS_EXTERNAL);

         assert(dep->src_subpass < pass->subpass_count);
         const struct vk_subpass *src_subpass =
            vk_render_pass_get_subpass(pass, dep->src_subpass);
         const struct vk_subpass *dst_subpass =
            vk_render_pass_get_subpass(pass, dep->dst_subpass);

         /* Figure out the set of views in the source subpass affected by this
          * dependency.
          */
         uint32_t src_dep_view_mask = dst_subpass->view_mask;
         if (dep->view_offset >= 0)
            src_dep_view_mask <<= dep->view_offset;
         else
            src_dep_view_mask >>= -dep->view_offset;

         /* From the Vulkan 1.3.204 spec:
          *
          *    "If the dependency is view-local, then each view (dstView) in
          *    the destination subpass depends on the view dstView +
          *    pViewOffsets[dependency] in the source subpass. If there is not
          *    such a view in the source subpass, then this dependency does
          *    not affect that view in the destination subpass."
          */
         if (!(src_subpass->view_mask & src_dep_view_mask))
            continue;
      }

      needs_mem_barrier = true;
      mem_barrier.srcStageMask |= dep->src_stage_mask;
      mem_barrier.srcAccessMask |= dep->src_access_mask;
      mem_barrier.dstStageMask |= dep->dst_stage_mask;
      mem_barrier.dstAccessMask |= dep->dst_access_mask;
   }

   if (subpass_idx == 0) {
      /* From the Vulkan 1.3.232 spec:
       *
       *    "If there is no subpass dependency from VK_SUBPASS_EXTERNAL to the
       *    first subpass that uses an attachment, then an implicit subpass
       *    dependency exists from VK_SUBPASS_EXTERNAL to the first subpass it
       *    is used in. The implicit subpass dependency only exists if there
       *    exists an automatic layout transition away from initialLayout. The
       *    subpass dependency operates as if defined with the following
       *    parameters:
       *
       *    VkSubpassDependency implicitDependency = {
       *        .srcSubpass = VK_SUBPASS_EXTERNAL;
       *        .dstSubpass = firstSubpass; // First subpass attachment is used in
       *        .srcStageMask = VK_PIPELINE_STAGE_NONE;
       *        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
       *        .srcAccessMask = 0;
       *        .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
       *                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
       *                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
       *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
       *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
       *        .dependencyFlags = 0;
       *    };"
       *
       * We could track individual subpasses and attachments and views to make
       * sure we only insert this barrier when it's absolutely necessary.
       * However, this is only going to happen for the first subpass and
       * you're probably going to take a stall in BeginRenderPass() anyway.
       * If this is ever a perf problem, we can re-evaluate and do something
       * more intellegent at that time.
       */
      needs_mem_barrier = true;
      mem_barrier.dstStageMask |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      mem_barrier.dstAccessMask |= VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   }

   uint32_t max_image_barrier_count = 0;

   for (uint32_t s = first_subpass; s < last_subpass; s++) {
      const struct vk_subpass *subpass =
         vk_render_pass_get_subpass(pass, s);
      struct vk_subpass_attachment_iter att_iter;

      vk_subpass_foreach_attachment(subpass, &att_iter, sp_att) {
         if (sp_att->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         assert(sp_att->attachment < pass->attachment_count);
         const struct vk_render_pass_attachment *rp_att =
            &pass->attachments[sp_att->attachment];

         max_image_barrier_count += util_bitcount(subpass->view_mask) *
                                    util_bitcount(rp_att->aspects);
      }

      if (pass->fragment_density_map.attachment != VK_ATTACHMENT_UNUSED)
         max_image_barrier_count += util_bitcount(subpass->view_mask);
   }

   STACK_ARRAY(VkImageMemoryBarrier2, image_barriers, max_image_barrier_count);
   uint32_t image_barrier_count = 0;

   for (uint32_t s = first_subpass; s < last_subpass; s++) {
      const struct vk_subpass *subpass =
         vk_render_pass_get_subpass(pass, s);
      struct vk_subpass_attachment_iter att_iter;

      vk_subpass_foreach_attachment(subpass, &att_iter, sp_att) {
         if (sp_att->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         /* If we're using an initial layout, the attachment will already be
          * marked as transitioned and this will be a no-op.
          */
         transition_attachment(cmd_buffer, sp_att->attachment,
                               subpass->view_mask,
                               sp_att->layout, sp_att->stencil_layout,
                               &image_barrier_count,
                               max_image_barrier_count,
                               image_barriers);
      }

      if (pass->fragment_density_map.attachment != VK_ATTACHMENT_UNUSED) {
         transition_attachment(cmd_buffer, pass->fragment_density_map.attachment,
                               subpass->view_mask,
                               pass->fragment_density_map.layout,
                               VK_IMAGE_LAYOUT_UNDEFINED,
                               &image_barrier_count,
                               max_image_barrier_count,
                               image_barriers);
      }
   }

   assert(image_barrier_count <= max_image_barrier_count);

   if (needs_mem_barrier || image_barrier_count > 0) {
      const VkDependencyInfo dependency_info = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .dependencyFlags = 0,
         .memoryBarrierCount = needs_mem_barrier ? 1 : 0,
         .pMemoryBarriers = needs_mem_barrier ? &mem_barrier : NULL,
         .imageMemoryBarrierCount = image_barrier_count,
         .pImageMemoryBarriers = image_barrier_count > 0 ?
                                 image_barriers : NULL,
      };
      cmd_buffer->runtime_rp_barrier = true;
      disp->CmdPipelineBarrier2(vk_command_buffer_to_handle(cmd_buffer),
                                &dependency_info);
      cmd_buffer->runtime_rp_barrier = false;
   }

   STACK_ARRAY_FINISH(image_barriers);
}

static void
subpass_load_attachments(struct vk_command_buffer *cmd_buffer)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;

   printf("%s:%i\n", __func__, __LINE__);
   for (uint32_t s = cmd_buffer->subpass_idx; s < pass->subpass_count; s++) {
      const struct vk_subpass *subpass = vk_render_pass_get_subpass(pass, s);
      struct vk_subpass_attachment_iter att_iter;

      printf("%s:%i subpass %d\n", __func__, __LINE__, s);
      vk_subpass_foreach_attachment(subpass, &att_iter, sp_att) {
         if (sp_att->attachment == VK_ATTACHMENT_UNUSED)
            continue;

         printf("%s:%i subpass %d att %d\n", __func__, __LINE__, s, sp_att->attachment);
         load_attachment(cmd_buffer, sp_att->attachment, subpass->view_mask,
                         sp_att->layout, sp_att->stencil_layout);
      }

      if (subpass->merged == VK_SUBPASS_NOT_MERGED ||
          subpass->merged == VK_SUBPASS_MERGED_LAST)
         break;
   }
   printf("%s:%i\n", __func__, __LINE__);

   /* TODO: Handle preserve attachments
    *
    * For immediate renderers, this isn't a big deal as LOAD_OP_LOAD and
    * STORE_OP_STORE are effectively free.  However, before this gets used on
    * a tiling GPU, we should really hook up preserve attachments and use them
    * to determine when we can use LOAD/STORE_OP_DONT_CARE between subpasses.
    */
}

static void
subpass_prepare_fsr_attachment(
   struct vk_command_buffer *cmd_buffer,
   VkRenderingInfo *rendering,
   VkRenderingFragmentShadingRateAttachmentInfoKHR *fsr_attachment)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, cmd_buffer->subpass_idx);

   if (!subpass->fragment_shading_rate_attachment)
      return;

   const struct vk_subpass_attachment *sp_att =
      subpass->fragment_shading_rate_attachment;

   assert(sp_att->attachment < pass->attachment_count);
   struct vk_attachment_state *att_state =
      &cmd_buffer->attachments[sp_att->attachment];

   /* Fragment shading rate attachments have no loadOp (it's implicitly
    * LOAD_OP_LOAD) so we need to ensure the load op happens.
    */
   load_attachment(cmd_buffer, sp_att->attachment, subpass->view_mask,
                   sp_att->layout, sp_att->stencil_layout);

   *fsr_attachment = (VkRenderingFragmentShadingRateAttachmentInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,
      .imageView = vk_image_view_to_handle(att_state->image_view),
      .imageLayout = sp_att->layout,
      .shadingRateAttachmentTexelSize =
         subpass->fragment_shading_rate_attachment_texel_size,
   };
   __vk_append_struct(rendering, fsr_attachment);
}

static void
subpass_prepare_fdm_attachment(
   struct vk_command_buffer *cmd_buffer,
   VkRenderingInfo *rendering,
   VkRenderingFragmentDensityMapAttachmentInfoEXT *fdm_attachment)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;

   if (pass->fragment_density_map.attachment == VK_ATTACHMENT_UNUSED)
      return;

   assert(pass->fragment_density_map.attachment < pass->attachment_count);
   struct vk_attachment_state *att_state =
      &cmd_buffer->attachments[pass->fragment_density_map.attachment];

   /* From the Vulkan 1.3.125 spec:
    *
    *    VUID-VkRenderPassFragmentDensityMapCreateInfoEXT-fragmentDensityMapAttachment-02550
    *
    *    If fragmentDensityMapAttachment is not VK_ATTACHMENT_UNUSED,
    *    fragmentDensityMapAttachment must reference an attachment with a
    *    loadOp equal to VK_ATTACHMENT_LOAD_OP_LOAD or
    *    VK_ATTACHMENT_LOAD_OP_DONT_CARE
    *
    * This means we don't have to implement the load op.
    */

   *fdm_attachment = (VkRenderingFragmentDensityMapAttachmentInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT,
      .imageView = vk_image_view_to_handle(att_state->image_view),
      .imageLayout = pass->fragment_density_map.layout,
   };
   __vk_append_struct(rendering, fdm_attachment);
}

static void
begin_subpass(struct vk_command_buffer *cmd_buffer,
              const VkSubpassBeginInfo *begin_info)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const struct vk_framebuffer *framebuffer = cmd_buffer->framebuffer;
   const uint32_t subpass_idx = cmd_buffer->subpass_idx;
   assert(subpass_idx < pass->subpass_count);
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, subpass_idx);
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;

   /* If we're inside a merged subpass range, all we have to do is emit
    * barriers for FB-local deps. Other deps for this subpass are handled in
    * the VK_SUBPASS_MERGED_FIRST subpass. */
   if (subpass->merged == VK_SUBPASS_MERGED_MID ||
       subpass->merged == VK_SUBPASS_MERGED_LAST) {
      begin_subpass_fb_local_barrier(cmd_buffer);
      return;
   }

   VkRenderingInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_INPUT_ATTACHMENT_NO_CONCURRENT_WRITES_BIT_MESA,
      .renderArea = cmd_buffer->render_area,
      .layerCount = pass->is_multiview ? 1 : framebuffer->layers,
      .viewMask = pass->is_multiview ? subpass->view_mask : 0,
   };

   /* First, we figure out all our attachments and attempt to handle image
    * layout transitions and load ops as part of vkCmdBeginRendering if we
    * can.  For any we can't handle this way, we'll need explicit barriers
    * or quick vkCmdBegin/EndRendering to do the load op.
    */

   STACK_ARRAY(VkRenderingAttachmentInfo, color_attachments,
               subpass->color_count);
   STACK_ARRAY(VkRenderingAttachmentInitialLayoutInfoMESA,
               color_attachment_initial_layouts,
               subpass->color_count);

   subpass_prepare_color_attachments(cmd_buffer, &rendering, color_attachments,
                                     color_attachment_initial_layouts);

   VkRenderingAttachmentInfo depth_attachment;
   VkRenderingAttachmentInfo stencil_attachment;
   VkRenderingAttachmentInitialLayoutInfoMESA depth_initial_layout;
   VkRenderingAttachmentInitialLayoutInfoMESA stencil_initial_layout;
   VkSampleLocationsInfoEXT sample_locations;

   subpass_prepare_ds_attachments(cmd_buffer, &rendering,
                                  &depth_attachment,
                                  &stencil_attachment,
                                  &depth_initial_layout,
                                  &stencil_initial_layout,
                                  &sample_locations);

   /* Next, handle any barriers we need.  This may include a general
    * VkMemoryBarrier for subpass dependencies and it may include some
    * number of VkImageMemoryBarriers for layout transitions.
    */
   begin_subpass_barriers(cmd_buffer);

   /* Next, handle any VK_ATTACHMENT_LOAD_OP_CLEAR that we couldn't handle
    * directly by emitting a quick vkCmdBegin/EndRendering to do the load.
    */
   subpass_load_attachments(cmd_buffer);

   if (subpass->legacy_dithering_enabled)
      rendering.flags |= VK_RENDERING_ENABLE_LEGACY_DITHERING_BIT_EXT;

   VkRenderingFragmentShadingRateAttachmentInfoKHR fsr_attachment;
   subpass_prepare_fsr_attachment(cmd_buffer, &rendering, &fsr_attachment);

   VkRenderingFragmentDensityMapAttachmentInfoEXT fdm_attachment;
   subpass_prepare_fdm_attachment(cmd_buffer, &rendering, &fdm_attachment);

   /* Append this one last because it lives in the subpass and we don't want
    * to be changed by appending other structures later.
    */
   if (subpass->mrtss.multisampledRenderToSingleSampledEnable)
      __vk_append_struct(&rendering, (void *)&subpass->mrtss);

   disp->CmdBeginRendering(vk_command_buffer_to_handle(cmd_buffer),
                           &rendering);

   STACK_ARRAY_FINISH(color_attachments);
   STACK_ARRAY_FINISH(color_attachment_initial_layouts);
}

static void
end_subpass(struct vk_command_buffer *cmd_buffer,
            const VkSubpassEndInfo *end_info)
{
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   const uint32_t subpass_idx = cmd_buffer->subpass_idx;
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;
   const struct vk_subpass *subpass =
      vk_render_pass_get_subpass(pass, subpass_idx);

   /* If we're merged with other subpasses and we're not the last in the group,
    * we don't want to stop the dynamic rendering pass. The last subpass in the
    * group will deal with external dependencies. */
   if (subpass->merged == VK_SUBPASS_MERGED_FIRST ||
       subpass->merged == VK_SUBPASS_MERGED_MID)
      return;

   disp->CmdEndRendering(vk_command_buffer_to_handle(cmd_buffer));

   uint32_t dep_src_start = subpass_idx;
   uint32_t dep_src_end = subpass_idx;
   bool needs_mem_barrier = false;
   VkMemoryBarrier2 mem_barrier = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
   };

   for (dep_src_start = subpass_idx; dep_src_start > 0 ; dep_src_start--) {
      subpass = vk_render_pass_get_subpass(pass, dep_src_start);

      if (subpass->merged != VK_SUBPASS_MERGED_LAST &&
          subpass->merged != VK_SUBPASS_MERGED_MID)
         break;
   }

   for (uint32_t d = 0; d < pass->dependency_count; d++) {
      const struct vk_subpass_dependency *dep = &pass->dependencies[d];
      if (dep->src_subpass >= dep_src_start && dep->src_subpass <= dep_src_end)
         continue;

      if (dep->dst_subpass != VK_SUBPASS_EXTERNAL)
         continue;

      needs_mem_barrier = true;
      mem_barrier.srcStageMask |= dep->src_stage_mask;
      mem_barrier.srcAccessMask |= dep->src_access_mask;
      mem_barrier.dstStageMask |= dep->dst_stage_mask;
      mem_barrier.dstAccessMask |= dep->dst_access_mask;
   }

   if (subpass_idx == pass->subpass_count - 1) {
      /* From the Vulkan 1.3.232 spec:
       *
       *    "Similarly, if there is no subpass dependency from the last
       *    subpass that uses an attachment to VK_SUBPASS_EXTERNAL, then an
       *    implicit subpass dependency exists from the last subpass it is
       *    used in to VK_SUBPASS_EXTERNAL. The implicit subpass dependency
       *    only exists if there exists an automatic layout transition into
       *    finalLayout. The subpass dependency operates as if defined with
       *    the following parameters:
       *
       *    VkSubpassDependency implicitDependency = {
       *        .srcSubpass = lastSubpass; // Last subpass attachment is used in
       *        .dstSubpass = VK_SUBPASS_EXTERNAL;
       *        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
       *        .dstStageMask = VK_PIPELINE_STAGE_NONE;
       *        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
       *                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
       *        .dstAccessMask = 0;
       *        .dependencyFlags = 0;
       *    };"
       *
       * We could track individual subpasses and attachments and views to make
       * sure we only insert this barrier when it's absolutely necessary.
       * However, this is only going to happen for the last subpass and
       * you're probably going to take a stall in EndRenderPass() anyway.
       * If this is ever a perf problem, we can re-evaluate and do something
       * more intellegent at that time.
       */
      needs_mem_barrier = true;
      mem_barrier.srcStageMask |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      mem_barrier.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
   }

   if (needs_mem_barrier) {
      const VkDependencyInfo dependency_info = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .dependencyFlags = 0,
         .memoryBarrierCount = 1,
         .pMemoryBarriers = &mem_barrier,
      };
      cmd_buffer->runtime_rp_barrier = true;
      disp->CmdPipelineBarrier2(vk_command_buffer_to_handle(cmd_buffer),
                                &dependency_info);
      cmd_buffer->runtime_rp_barrier = false;
   }
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                              const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                              const VkSubpassBeginInfo *pSubpassBeginInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(vk_render_pass, pass, pRenderPassBeginInfo->renderPass);
   VK_FROM_HANDLE(vk_framebuffer, framebuffer,
                  pRenderPassBeginInfo->framebuffer);

   assert(cmd_buffer->render_pass == NULL);
   cmd_buffer->render_pass = pass;
   cmd_buffer->subpass_idx = 0;

   assert(cmd_buffer->framebuffer == NULL);
   cmd_buffer->framebuffer = framebuffer;

   cmd_buffer->render_area = pRenderPassBeginInfo->renderArea;

   assert(cmd_buffer->attachments == NULL);
   if (pass->attachment_count > ARRAY_SIZE(cmd_buffer->_attachments)) {
      cmd_buffer->attachments = malloc(pass->attachment_count *
                                       sizeof(*cmd_buffer->attachments));
   } else {
      cmd_buffer->attachments = cmd_buffer->_attachments;
   }

   const VkRenderPassAttachmentBeginInfo *attach_begin =
      vk_find_struct_const(pRenderPassBeginInfo,
                           RENDER_PASS_ATTACHMENT_BEGIN_INFO);
   if (!attach_begin)
      assert(pass->attachment_count == framebuffer->attachment_count);

   const VkImageView *image_views;
   if (attach_begin && attach_begin->attachmentCount != 0) {
      assert(attach_begin->attachmentCount == pass->attachment_count);
      image_views = attach_begin->pAttachments;
   } else {
      assert(framebuffer->attachment_count >= pass->attachment_count);
      image_views = framebuffer->attachments;
   }

   for (uint32_t a = 0; a < pass->attachment_count; ++a) {
      VK_FROM_HANDLE(vk_image_view, image_view, image_views[a]);
      const struct vk_render_pass_attachment *pass_att = &pass->attachments[a];
      struct vk_attachment_state *att_state = &cmd_buffer->attachments[a];

      /* From the Vulkan 1.3.204 spec:
       *
       *    VUID-VkFramebufferCreateInfo-pAttachments-00880
       *
       *    "If renderpass is not VK_NULL_HANDLE and flags does not include
       *    VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, each element of pAttachments
       *    must have been created with a VkFormat value that matches the
       *    VkFormat specified by the corresponding VkAttachmentDescription in
       *    renderPass"
       *
       * and
       *
       *    VUID-VkRenderPassBeginInfo-framebuffer-03216
       *
       *    "If framebuffer was created with a VkFramebufferCreateInfo::flags
       *    value that included VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, each
       *    element of the pAttachments member of a
       *    VkRenderPassAttachmentBeginInfo structure included in the pNext
       *    chain must be a VkImageView of an image created with a value of
       *    VkImageViewCreateInfo::format equal to the corresponding value of
       *    VkAttachmentDescription::format in renderPass"
       */
      assert(image_view->format == pass_att->format);

      /* From the Vulkan 1.3.204 spec:
       *
       *    VUID-VkFramebufferCreateInfo-pAttachments-00881
       *
       *    "If renderpass is not VK_NULL_HANDLE and flags does not include
       *    VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, each element of pAttachments
       *    must have been created with a samples value that matches the
       *    samples value specified by the corresponding
       *    VkAttachmentDescription in renderPass"
       *
       * and
       *
       *    UID-VkRenderPassBeginInfo-framebuffer-03217
       *
       *    "If framebuffer was created with a VkFramebufferCreateInfo::flags
       *    value that included VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT, each
       *    element of the pAttachments member of a
       *    VkRenderPassAttachmentBeginInfo structure included in the pNext
       *    chain must be a VkImageView of an image created with a value of
       *    VkImageCreateInfo::samples equal to the corresponding value of
       *    VkAttachmentDescription::samples in renderPass"
       */
      assert(image_view->image->samples == pass_att->samples);

      /* From the Vulkan 1.3.204 spec:
       *
       *    If multiview is enabled and the shading rate attachment has
       *    multiple layers, the shading rate attachment texel is selected
       *    from the layer determined by the ViewIndex built-in. If multiview
       *    is disabled, and both the shading rate attachment and the
       *    framebuffer have multiple layers, the shading rate attachment
       *    texel is selected from the layer determined by the Layer built-in.
       *    Otherwise, the texel is unconditionally selected from the first
       *    layer of the attachment.
       */
      if (!(image_view->usage & VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR))
         assert(util_last_bit(pass_att->view_mask) <= image_view->layer_count);

      *att_state = (struct vk_attachment_state) {
         .image_view = image_view,
         .views_loaded = 0,
      };

      for (uint32_t v = 0; v < MESA_VK_MAX_MULTIVIEW_VIEW_COUNT; v++) {
         att_state->views[v] = (struct vk_attachment_view_state) {
            .layout = pass_att->initial_layout,
            .stencil_layout = pass_att->initial_stencil_layout,
         };
      }

      if (a < pRenderPassBeginInfo->clearValueCount)
         att_state->clear_value = pRenderPassBeginInfo->pClearValues[a];
   }

   const VkRenderPassSampleLocationsBeginInfoEXT *rp_sl_info =
      vk_find_struct_const(pRenderPassBeginInfo->pNext,
                           RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT);
   if (rp_sl_info) {
      cmd_buffer->pass_sample_locations = clone_rp_sample_locations(rp_sl_info);
      assert(cmd_buffer->pass_sample_locations);

      for (uint32_t i = 0; i < rp_sl_info->attachmentInitialSampleLocationsCount; i++) {
         const VkAttachmentSampleLocationsEXT *att_sl =
            &rp_sl_info->pAttachmentInitialSampleLocations[i];

         assert(att_sl->attachmentIndex < pass->attachment_count);
         struct vk_attachment_state *att_state =
            &cmd_buffer->attachments[att_sl->attachmentIndex];

         /* Sample locations only matter for depth/stencil images created with
          * VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT
          */
         if (vk_format_is_depth_or_stencil(att_state->image_view->format) &&
             (att_state->image_view->image->create_flags &
              VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT)) {
            for (uint32_t v = 0; v < MESA_VK_MAX_MULTIVIEW_VIEW_COUNT; v++)
               att_state->views[v].sample_locations = &att_sl->sampleLocationsInfo;
         }
      }
   }

   begin_subpass(cmd_buffer, pSubpassBeginInfo);
}

void
vk_command_buffer_reset_render_pass(struct vk_command_buffer *cmd_buffer)
{
   cmd_buffer->render_pass = NULL;
   cmd_buffer->subpass_idx = 0;
   cmd_buffer->framebuffer = NULL;
   if (cmd_buffer->attachments != cmd_buffer->_attachments)
      free(cmd_buffer->attachments);
   cmd_buffer->attachments = NULL;
   if (cmd_buffer->pass_sample_locations != NULL)
      vk_free(vk_default_allocator(), cmd_buffer->pass_sample_locations);
   cmd_buffer->pass_sample_locations = NULL;
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdNextSubpass2(VkCommandBuffer commandBuffer,
                          const VkSubpassBeginInfo *pSubpassBeginInfo,
                          const VkSubpassEndInfo *pSubpassEndInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);

   end_subpass(cmd_buffer, pSubpassEndInfo);
   cmd_buffer->subpass_idx++;
   begin_subpass(cmd_buffer, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL
vk_common_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                            const VkSubpassEndInfo *pSubpassEndInfo)
{
   VK_FROM_HANDLE(vk_command_buffer, cmd_buffer, commandBuffer);
   const struct vk_render_pass *pass = cmd_buffer->render_pass;
   struct vk_device_dispatch_table *disp =
      &cmd_buffer->base.device->dispatch_table;

   end_subpass(cmd_buffer, pSubpassEndInfo);

   /* Make sure all our attachments end up in their finalLayout */

   uint32_t max_image_barrier_count = 0;
   for (uint32_t a = 0; a < pass->attachment_count; a++) {
      const struct vk_render_pass_attachment *rp_att = &pass->attachments[a];

      max_image_barrier_count += util_bitcount(pass->view_mask) *
                                 util_bitcount(rp_att->aspects);
   }
   STACK_ARRAY(VkImageMemoryBarrier2, image_barriers, max_image_barrier_count);
   uint32_t image_barrier_count = 0;

   for (uint32_t a = 0; a < pass->attachment_count; a++) {
      const struct vk_render_pass_attachment *rp_att = &pass->attachments[a];

      transition_attachment(cmd_buffer, a, pass->view_mask,
                            rp_att->final_layout,
                            rp_att->final_stencil_layout,
                            &image_barrier_count,
                            max_image_barrier_count,
                            image_barriers);
   }
   assert(image_barrier_count <= max_image_barrier_count);

   if (image_barrier_count > 0) {
      const VkDependencyInfo dependency_info = {
         .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
         .dependencyFlags = 0,
         .imageMemoryBarrierCount = image_barrier_count,
         .pImageMemoryBarriers = image_barriers,
      };
      cmd_buffer->runtime_rp_barrier = true;
      disp->CmdPipelineBarrier2(vk_command_buffer_to_handle(cmd_buffer),
                                &dependency_info);
      cmd_buffer->runtime_rp_barrier = false;
   }

   STACK_ARRAY_FINISH(image_barriers);

   vk_command_buffer_reset_render_pass(cmd_buffer);
}
