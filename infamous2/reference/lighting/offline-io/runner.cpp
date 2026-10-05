// Standalone, offline compute runner: runner input.bin output.bin iterations sizes output pass.spv:x:y:z ...
// Storage buffer 0 holds input.bin; "sizes" are the byte sizes of buffers 1, 2, ... separated by commas and "output"
// is the buffer written to output.bin. Runs the passes in order (with a barrier between them) and reports the time
// per iteration. Does not attach to RPCS3.
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult r) { if (r != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(r)); }

static std::vector<char> read(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot read " + path);
    std::vector<char> b(f.tellg());
    f.seekg(0);
    f.read(b.data(), b.size());
    return b;
}

struct pass_t { VkPipeline pipeline; VkShaderModule module; uint32_t groups[3]; };

int main(int argc, char** argv) try
{
    if (argc < 7) throw std::runtime_error("runner input.bin output.bin iterations sizes output pass.spv:x:y:z ...");
    const auto input = read(argv[1]);
    const int iterations = std::stoi(argv[3]);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    check(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t n = 0;
    check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> physical(n);
    check(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t family = 0;
    for (auto gpu : physical)
    {
        VkPhysicalDeviceProperties prop;
        vkGetPhysicalDeviceProperties(gpu, &prop);
        if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
        for (uint32_t i = 0; i < count && !selected; i++)
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { selected = gpu; family = i; }
        if (selected) break;
    }
    if (!selected) throw std::runtime_error("No hardware Vulkan compute device");

    float priority = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device;
    check(vkCreateDevice(selected, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    std::vector<VkDeviceSize> sizes{input.size()};
    {
        std::stringstream list(argv[4]);
        for (std::string part; std::getline(list, part, ',');) sizes.push_back(std::stoull(part));
    }
    const uint32_t buffer_count = sizes.size(), output = std::stoul(argv[5]);
    if (output >= buffer_count) throw std::runtime_error("No such output buffer");
    std::vector<VkBuffer> buffers(buffer_count);
    std::vector<VkDeviceMemory> memory(buffer_count);
    std::vector<void*> maps(buffer_count);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(selected, &mp);
    constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < buffer_count; i++)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizes[i]; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device, &bci, nullptr, &buffers[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffers[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t j = 0; j < mp.memoryTypeCount && type == UINT32_MAX; j++)
            if ((req.memoryTypeBits & (1u << j)) && (mp.memoryTypes[j].propertyFlags & wanted) == wanted) type = j;
        if (type == UINT32_MAX) throw std::runtime_error("No coherent host-visible storage memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size; ai.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &ai, nullptr, &memory[i]));
        check(vkBindBufferMemory(device, buffers[i], memory[i], 0));
        check(vkMapMemory(device, memory[i], 0, sizes[i], 0, &maps[i]));
        std::memset(maps[i], 0, sizes[i]);
    }
    std::memcpy(maps[0], input.data(), input.size());

    std::vector<VkDescriptorSetLayoutBinding> bindings(buffer_count);
    for (uint32_t i = 0; i < buffer_count; i++) bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    slci.bindingCount = buffer_count; slci.pBindings = bindings.data();
    VkDescriptorSetLayout sl;
    check(vkCreateDescriptorSetLayout(device, &slci, nullptr, &sl));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &sl;
    VkPipelineLayout layout;
    check(vkCreatePipelineLayout(device, &plci, nullptr, &layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer_count};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    VkDescriptorPool pool;
    check(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = pool; sai.descriptorSetCount = 1; sai.pSetLayouts = &sl;
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &sai, &set));
    std::vector<VkDescriptorBufferInfo> infos(buffer_count);
    std::vector<VkWriteDescriptorSet> writes(buffer_count);
    for (uint32_t i = 0; i < buffer_count; i++)
    {
        infos[i] = {buffers[i], 0, sizes[i]};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, buffer_count, writes.data(), 0, nullptr);

    std::vector<pass_t> passes;
    for (int i = 6; i < argc; i++)
    {
        std::stringstream spec(argv[i]);
        std::string path, part;
        std::getline(spec, path, ':');
        pass_t pass{};
        for (auto& g : pass.groups) { std::getline(spec, part, ':'); g = std::stoul(part); }
        const auto shader = read(path);
        std::vector<uint32_t> words((shader.size() + 3) / 4);
        std::memcpy(words.data(), shader.data(), shader.size());
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = shader.size(); smci.pCode = words.data();
        check(vkCreateShaderModule(device, &smci, nullptr, &pass.module));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pci.layout = layout;
        pci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module = pass.module; pci.stage.pName = "main";
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &pass.pipeline));
        passes.push_back(pass);
    }

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family;
    VkCommandPool command_pool;
    check(vkCreateCommandPool(device, &cpci, nullptr, &command_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = command_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    for (const auto& pass : passes)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
        vkCmdDispatch(cmd, pass.groups[0], pass.groups[1], pass.groups[2]);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    check(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    const auto run = [&](int count)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < count; i++) { check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue)); }
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / count;
    };
    run(1);
    if (iterations > 1)
    {
        run(iterations / 4 + 1); // let the GPU clock settle
        std::cout << "ms_per_iteration " << run(iterations) << '\n';
    }

    std::ofstream(argv[2], std::ios::binary).write(static_cast<char*>(maps[output]), sizes[output]);
    vkDestroyCommandPool(device, command_pool, nullptr);
    for (auto& pass : passes) { vkDestroyPipeline(device, pass.pipeline, nullptr); vkDestroyShaderModule(device, pass.module, nullptr); }
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, sl, nullptr);
    for (uint32_t i = 0; i < buffer_count; i++) { vkUnmapMemory(device, memory[i]); vkDestroyBuffer(device, buffers[i], nullptr); vkFreeMemory(device, memory[i], nullptr); }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
