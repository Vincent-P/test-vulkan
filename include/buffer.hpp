#pragma once

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.hpp>

namespace my_app
{
    struct VulkanContext;

    class Buffer
    {
        public:
        Buffer();
        Buffer(const VulkanContext& vulkan, size_t _size, vk::BufferUsageFlags _buf_usage, const char* name, VmaMemoryUsage _mem_usage = VMA_MEMORY_USAGE_CPU_TO_GPU);
        Buffer(const VulkanContext& vulkan, size_t _size, vk::BufferUsageFlags _buf_usage, VmaMemoryUsage _mem_usage = VMA_MEMORY_USAGE_CPU_TO_GPU);

        void free();
        void* map();
        void unmap();
        void flush();
        void set_name(std::string new_name);

        vk::Buffer get_buffer() const { return buffer; }

        vk::DescriptorBufferInfo get_desc_info() const
        {
            vk::DescriptorBufferInfo dbi{};
            dbi.buffer = buffer;
            dbi.offset = 0;
            dbi.range = size;
            return dbi;
        }

        size_t get_size() const { return size; }

        vk::BufferUsageFlags get_buf_usage() const { return buf_usage; }

        VmaMemoryUsage get_mem_usage() const { return mem_usage; }

        private:
        const VmaAllocator* allocator;
        void* mapped;
        size_t size;
        vk::BufferUsageFlags buf_usage;
        VmaMemoryUsage mem_usage;
        vk::Buffer buffer;
        VmaAllocation allocation;
    };
}    // namespace my_app
