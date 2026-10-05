// Standalone, offline compute runner. Does not attach to RPCS3 or write sysfs.
#include <vulkan/vulkan.h>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>

void check(VkResult r) { if (r != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(r)); }
std::vector<char> read(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(std::string("Cannot read ") + path);
    std::vector<char> b(f.tellg()); f.seekg(0); f.read(b.data(), b.size()); return b;
}
int main(int argc, char** argv) try
{
    if (argc != 5) throw std::runtime_error("runner input.bin tiles.spv lighting.spv output.bin");
    auto input = read(argv[1]);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance; check(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n=0; check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
    std::vector<VkPhysicalDevice> physical(n); check(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
    VkPhysicalDevice selected = VK_NULL_HANDLE; uint32_t family=0;
    for (auto gpu : physical)
    {
        VkPhysicalDeviceProperties prop; vkGetPhysicalDeviceProperties(gpu, &prop);
        if (prop.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,families.data());
        for (uint32_t i=0;i<count;i++) if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
        { selected=gpu; family=i; std::cout << prop.deviceName << '\n'; break; }
        if (selected) break;
    }
    if (!selected) throw std::runtime_error("No hardware Vulkan compute device");
    float priority=1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    VkDevice device; check(vkCreateDevice(selected,&dci,nullptr,&device));
    VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    constexpr uint32_t pixels=1280*720;
    const std::array<VkDeviceSize,4> sizes{input.size(),pixels*8,pixels*16,720*4*8};
    std::array<VkBuffer,4> buffers{}; std::array<VkDeviceMemory,4> memory{}; std::array<void*,4> maps{};
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(selected,&mp);
    for (unsigned i=0;i<4;i++)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size=sizes[i]; bci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device,&bci,nullptr,&buffers[i]));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffers[i],&req);
        uint32_t type=UINT32_MAX;
        for (uint32_t j=0;j<mp.memoryTypeCount;j++)
            if ((req.memoryTypeBits&(1u<<j)) && (mp.memoryTypes[j].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=j; break; }
        if (type==UINT32_MAX) throw std::runtime_error("No coherent host-visible storage memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        check(vkAllocateMemory(device,&ai,nullptr,&memory[i])); check(vkBindBufferMemory(device,buffers[i],memory[i],0));
        check(vkMapMemory(device,memory[i],0,sizes[i],0,&maps[i]));
        std::memset(maps[i],0,sizes[i]);
    }
    std::memcpy(maps[0],input.data(),input.size());
    std::array<VkDescriptorSetLayoutBinding,4> bindings{};
    for(unsigned i=0;i<4;i++) bindings[i]={i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo slci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; slci.bindingCount=4; slci.pBindings=bindings.data();
    VkDescriptorSetLayout sl; check(vkCreateDescriptorSetLayout(device,&slci,nullptr,&sl));
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; plci.setLayoutCount=1; plci.pSetLayouts=&sl;
    VkPipelineLayout layout; check(vkCreatePipelineLayout(device,&plci,nullptr,&layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,4};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&ps;
    VkDescriptorPool pool; check(vkCreateDescriptorPool(device,&dpci,nullptr,&pool));
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sai.descriptorPool=pool; sai.descriptorSetCount=1; sai.pSetLayouts=&sl;
    VkDescriptorSet set; check(vkAllocateDescriptorSets(device,&sai,&set));
    std::array<VkDescriptorBufferInfo,4> infos{}; std::array<VkWriteDescriptorSet,4> writes{};
    for(unsigned i=0;i<4;i++)
    {
        infos[i]={buffers[i],0,sizes[i]}; writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&infos[i];
    }
    vkUpdateDescriptorSets(device,4,writes.data(),0,nullptr);
    std::array<VkShaderModule,2> modules{}; std::array<VkPipeline,2> pipelines{};
    for(unsigned i=0;i<2;i++)
    {
        auto shader=read(argv[i+2]);
        std::vector<uint32_t> words((shader.size()+3)/4); std::memcpy(words.data(),shader.data(),shader.size());
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smci.codeSize=shader.size(); smci.pCode=words.data();
        check(vkCreateShaderModule(device,&smci,nullptr,&modules[i]));
        VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.layout=layout;
        pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; pci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pci.stage.module=modules[i]; pci.stage.pName="main";
        check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipelines[i]));
    }
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=family;
    VkCommandPool command_pool; check(vkCreateCommandPool(device,&cpci,nullptr,&command_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=command_pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cmd; check(vkAllocateCommandBuffers(device,&cai,&cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; check(vkBeginCommandBuffer(cmd,&begin));
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[0]); vkCmdDispatch(cmd,(720+63)/64,1,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[1]); vkCmdDispatch(cmd,(pixels+63)/64,1,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    check(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    check(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); check(vkQueueWaitIdle(queue));
    std::ofstream out(argv[4],std::ios::binary); out.write(static_cast<char*>(maps[1]),sizes[1]); out.close();
    std::ofstream positions(std::string(argv[4])+".positions",std::ios::binary); positions.write(static_cast<char*>(maps[2]),sizes[2]); positions.close();
    vkDestroyCommandPool(device,command_pool,nullptr);
    for(unsigned i=0;i<2;i++) { vkDestroyPipeline(device,pipelines[i],nullptr); vkDestroyShaderModule(device,modules[i],nullptr); }
    vkDestroyDescriptorPool(device,pool,nullptr); vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorSetLayout(device,sl,nullptr);
    for(unsigned i=0;i<4;i++) { vkUnmapMemory(device,memory[i]); vkDestroyBuffer(device,buffers[i],nullptr); vkFreeMemory(device,memory[i],nullptr); }
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    return 0;
}
catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
