/*
 * Copyright © 2025 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "helpers.h"

class misc : public radv_test {};

/**
 * This test verifies that the pipeline cache UUID is invariant when random debug options or
 * workarounds are applied. This is very important for SteamOS precompilation.
 */
TEST_F(misc, invariant_pipeline_cache_uuid)
{
   create_device();

   VkPhysicalDeviceProperties2 pdev_props_default = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
   };

   get_physical_device_properties2(&pdev_props_default);

   const uint8_t *uuid_default = pdev_props_default.properties.pipelineCacheUUID;

   destroy_device();

   setenv("radv_disable_shrink_image_store", "true", 1);
   setenv("radv_lower_terminate_to_discard", "true", 1);
   setenv("RADV_DEBUG", "cswave32", true);

   create_device();

   VkPhysicalDeviceProperties2 pdev_props_override = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
   };

   get_physical_device_properties2(&pdev_props_override);

   const uint8_t *uuid_override = pdev_props_override.properties.pipelineCacheUUID;

   EXPECT_TRUE(!memcmp(uuid_default, uuid_override, VK_UUID_SIZE));

   unsetenv("radv_disable_shrink_image_store");
   unsetenv("radv_lower_terminate_to_discard");
   unsetenv("RADV_DEBUG");

   destroy_device();
}
