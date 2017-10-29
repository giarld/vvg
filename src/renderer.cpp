#include "vvg.hpp"
#include "nanovg_vk.h"
#include "nanovg.h"

// vpp
#include <vpp/bufferOps.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/swapchain.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/surface.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/vk.hpp>
#include <vpp/util/file.hpp>

#include <dlg/dlg.hpp>

// stl
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <iterator>

// shader header
#include "shader/fill.frag.h"
#include "shader/fill.vert.h"

namespace vvg {

template<typename... T> constexpr void unused(T&&...) {}

// minimal shader typedefs
struct Vec2 { float x,y; };
struct Vec3 { float x,y,z; };
struct Vec4 { float x,y,z,w; };

using Mat2 = float[2][2];
using Mat3 = float[3][3];
using Mat4 = float[4][4];

struct UniformData {
	Vec2 viewSize;
	std::uint32_t type;
	std::uint32_t texType;
	Vec4 innerColor;
	Vec4 outerColor;
	Mat4 scissorMat;
	Mat4 paintMat;
};

struct Path {
	std::size_t fillOffset = 0;
	std::size_t fillCount = 0;
	std::size_t strokeOffset = 0;
	std::size_t strokeCount = 0;
};

struct DrawData {
	vpp::DescriptorSet descriptorSet;
	UniformData uniformData;
	unsigned int texture = 0;

	std::vector<Path> paths;
	std::size_t triangleOffset = 0;
	std::size_t triangleCount = 0;
};

// The RenderBuilder implementation used to render on a swapchain.
struct RenderImpl : public vpp::RendererBuilder {
	std::vector<vk::ClearValue> clearValues(unsigned int id) override;
	void build(unsigned int id, const vpp::RenderPassInstance& ini) override;
	void frame(unsigned int id) override;

	Renderer* renderer;
	vpp::SwapchainRenderer* swapchainRenderer;
};

} // namespace vvg

// vpp VulkanType specializations for our own shader types
namespace vpp {

template<> struct VulkanType<vvg::Vec2> : public VulkanTypeVec<2> {};
template<> struct VulkanType<vvg::Vec3> : public VulkanTypeVec<3> {};
template<> struct VulkanType<vvg::Vec4> : public VulkanTypeVec<4> {};
template<> struct VulkanType<vvg::Mat2> : public VulkanTypeMat<2, 2, true> {};
template<> struct VulkanType<vvg::Mat3> : public VulkanTypeMat<3, 3, true> {};
template<> struct VulkanType<vvg::Mat4> : public VulkanTypeMat<4, 4, true> {};

template<> struct VulkanType<vvg::UniformData> {
	static constexpr auto type = vpp::ShaderType::structure;
	static constexpr auto align = true;
	static constexpr auto members = std::make_tuple(
		&vvg::UniformData::viewSize,
		&vvg::UniformData::type,
		&vvg::UniformData::texType,
		&vvg::UniformData::innerColor,
		&vvg::UniformData::outerColor,
		&vvg::UniformData::scissorMat,
		&vvg::UniformData::paintMat
	);
};

}


namespace vvg {

//Renderer
Renderer::Renderer(const vpp::Swapchain& swapchain, const vpp::Queue* presentQueue)
	: vpp::Resource(swapchain.device()), swapchain_(&swapchain), presentQueue_(presentQueue)
{
	initRenderPass(swapchain.device(), swapchain.format());
	init();

	auto impl = std::make_unique<RenderImpl>();
	impl->renderer = this;
	impl->swapchainRenderer = &renderer_;

	auto attachmentInfo = vpp::ViewableImage::defaultDepth2D();
	attachmentInfo.imgInfo.format = vk::Format::s8Uint;
	attachmentInfo.viewInfo.format = vk::Format::s8Uint;
	attachmentInfo.viewInfo.subresourceRange.aspectMask = vk::ImageAspectBits::stencil;

	vpp::SwapchainRenderer::CreateInfo info {renderPass_, 0, {{attachmentInfo}}};
	renderer_ = {swapchain, info, std::move(impl)};
}

Renderer::Renderer(const vpp::Framebuffer& framebuffer, vk::RenderPass rp)
	: vpp::Resource(framebuffer.device()), framebuffer_(&framebuffer), renderPassHandle_(rp)
{
	init();
	commandBuffer_ = framebuffer.device().commandProvider().get(renderQueue_->family());
}


Renderer::~Renderer()
{

}

void Renderer::init()
{
	// queues
	renderQueue_ = device().queue(vk::QueueBits::graphics);

	if(swapchain_ && !presentQueue_) {
		auto surface = swapchain_->vkSurface();
		auto supported = vpp::supportedQueueFamilies(vkInstance(), surface, vkPhysicalDevice());
		for(auto q : supported)
			if((presentQueue_ = device().queue(q)))
				break;

		if(!presentQueue_)
			throw std::runtime_error("vvg::Renderer::init: cannot find present queue");
	}

	// sampler
	vk::SamplerCreateInfo samplerInfo;
	samplerInfo.magFilter = vk::Filter::linear;
	samplerInfo.minFilter = vk::Filter::linear;
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::linear;
	samplerInfo.addressModeU = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeV = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeW = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.mipLodBias = 0;
	samplerInfo.anisotropyEnable = true;
	samplerInfo.maxAnisotropy = 1;
	samplerInfo.compareEnable = false;
	samplerInfo.compareOp = {};
	samplerInfo.minLod = 0;
	samplerInfo.maxLod = 0;
	samplerInfo.borderColor = vk::BorderColor::floatTransparentBlack;
	samplerInfo.unnormalizedCoordinates = false;
	sampler_ = {device(), samplerInfo};

	// descLayout
	auto descriptorBindings  = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle())
	};

	descriptorLayout_ = {device(), descriptorBindings};
	pipelineLayout_ = {device(), {descriptorLayout_}, {}};

	//create the graphics pipeline
	// vpp::GraphicsPipelineBuilder builder(device(), vkRenderPass());
	// builder.dynamicStates = {vk::DynamicState::viewport, vk::DynamicState::scissor};
	// builder.states.rasterization.cullMode = vk::CullModeBits::none;
	// builder.states.inputAssembly.topology = vk::PrimitiveTopology::triangleFan;
	// builder.states.blendAttachments[0].blendEnable = true;
	// builder.states.blendAttachments[0].colorBlendOp = vk::BlendOp::add;
	// builder.states.blendAttachments[0].srcColorBlendFactor = vk::BlendFactor::srcAlpha;
	// builder.states.blendAttachments[0].dstColorBlendFactor = vk::BlendFactor::oneMinusSrcAlpha;
	// builder.states.blendAttachments[0].srcAlphaBlendFactor = vk::BlendFactor::one;
	// builder.states.blendAttachments[0].dstAlphaBlendFactor = vk::BlendFactor::zero;
	// builder.states.blendAttachments[0].alphaBlendOp = vk::BlendOp::add;
	//
	// vpp::VertexBufferLayout vertexLayout {{vk::Format::r32g32Sfloat, vk::Format::r32g32Sfloat}};
	// builder.vertexBufferLayouts = {vertexLayout};
	// builder.layout = pipelineLayout_;
	//
	// //shader
	// //the fragment shader has a constant for antialiasing
	// std::uint32_t antiAliasing = edgeAA_;
	// vk::SpecializationMapEntry entry {0, 0, 4};
	//
	// vk::SpecializationInfo specInfo;
	// specInfo.mapEntryCount = 1;
	// specInfo.pMapEntries = &entry;
	// specInfo.dataSize = 4;
	// specInfo.pData = &antiAliasing;
	//
	// builder.shader.stage(fill_vert_data, {vk::ShaderStageBits::vertex});
	// builder.shader.stage(fill_frag_data, {vk::ShaderStageBits::fragment, &specInfo});
	//
	// fanPipeline_ = builder.build();
	//
	// builder.states.inputAssembly.topology = vk::PrimitiveTopology::triangleStrip;
	// stripPipeline_ = builder.build();
	//
	// builder.states.inputAssembly.topology = vk::PrimitiveTopology::triangleList;
	// listPipeline_ = builder.build();


	std::uint32_t antiAliasing = edgeAA_;
	vk::SpecializationMapEntry entry {0, 0, 4};

	vk::SpecializationInfo specInfo;
	specInfo.mapEntryCount = 1;
	specInfo.pMapEntries = &entry;
	specInfo.dataSize = 4;
	specInfo.pData = &antiAliasing;

	vpp::ShaderModule vertexShader(device(), fill_vert_data);
	vpp::ShaderModule fragmentShader(device(), fill_frag_data);

	vpp::ShaderProgram shaderStages({
		{vertexShader, vk::ShaderStageBits::vertex},
		{fragmentShader, vk::ShaderStageBits::fragment, &specInfo}
	});

	vk::GraphicsPipelineCreateInfo pipelineInfo;
	pipelineInfo.renderPass = vkRenderPass();
	pipelineInfo.layout = pipelineLayout_;

	pipelineInfo.stageCount = shaderStages.vkStageInfos().size();
	pipelineInfo.pStages = shaderStages.vkStageInfos().data();

	constexpr auto stride = (2 * 4) * 2; // 2 pos floats, 2 uv floats
	vk::VertexInputBindingDescription bufferBinding {0, stride, vk::VertexInputRate::vertex};

	// vertex position, uv attributes
	vk::VertexInputAttributeDescription attributes[2];
	attributes[0].format = vk::Format::r32g32Sfloat;

	attributes[1].location = 1;
	attributes[1].format = vk::Format::r32g32Sfloat;
	attributes[1].offset = 2 * 4; // offset pos (vec2f)

	vk::PipelineVertexInputStateCreateInfo vertexInfo;
	vertexInfo.vertexBindingDescriptionCount = 1;
	vertexInfo.pVertexBindingDescriptions = &bufferBinding;
	vertexInfo.vertexAttributeDescriptionCount = 2;
	vertexInfo.pVertexAttributeDescriptions = attributes;
	pipelineInfo.pVertexInputState = &vertexInfo;

	vk::PipelineInputAssemblyStateCreateInfo assemblyInfo;
	assemblyInfo.topology = vk::PrimitiveTopology::triangleList;
	pipelineInfo.pInputAssemblyState = &assemblyInfo;

	vk::PipelineRasterizationStateCreateInfo rasterizationInfo;
	rasterizationInfo.polygonMode = vk::PolygonMode::fill;
	rasterizationInfo.cullMode = vk::CullModeBits::none;
	rasterizationInfo.frontFace = vk::FrontFace::counterClockwise;
	rasterizationInfo.depthClampEnable = false;
	rasterizationInfo.rasterizerDiscardEnable = false;
	rasterizationInfo.depthBiasEnable = false;
	rasterizationInfo.lineWidth = 1.f;
	pipelineInfo.pRasterizationState = &rasterizationInfo;

	vk::PipelineMultisampleStateCreateInfo multisampleInfo;
	multisampleInfo.rasterizationSamples = vk::SampleCountBits::e1;
	pipelineInfo.pMultisampleState = &multisampleInfo;

	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.alphaBlendOp = vk::BlendOp::add;
	blendAttachment.colorBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::srcAlpha;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::oneMinusSrcAlpha;
	blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::zero;
	blendAttachment.colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	vk::PipelineColorBlendStateCreateInfo blendInfo;
	blendInfo.attachmentCount = 1;
	blendInfo.pAttachments = &blendAttachment;
	pipelineInfo.pColorBlendState = &blendInfo;

	vk::PipelineViewportStateCreateInfo viewportInfo;
	viewportInfo.scissorCount = 1;
	viewportInfo.viewportCount = 1;
	pipelineInfo.pViewportState = &viewportInfo;

	vk::PipelineDepthStencilStateCreateInfo depthStencilInfo;
	pipelineInfo.pDepthStencilState = &depthStencilInfo;

	constexpr auto dynStates = {vk::DynamicState::viewport, vk::DynamicState::scissor};

	vk::PipelineDynamicStateCreateInfo dynamicInfo;
	dynamicInfo.dynamicStateCount = dynStates.size();
	dynamicInfo.pDynamicStates = dynStates.begin();
	pipelineInfo.pDynamicState = &dynamicInfo;

	// copy for strip pipeline
	auto stripInfo = pipelineInfo;
	auto stripAssembly = assemblyInfo;
	stripAssembly.topology = vk::PrimitiveTopology::triangleStrip;
	stripInfo.pInputAssemblyState = &stripAssembly;
	stripInfo.basePipelineIndex = 0;

	// copy for fan pipelien
	auto fanInfo = pipelineInfo;
	auto fanAssembly = assemblyInfo;
	fanAssembly.topology = vk::PrimitiveTopology::triangleFan;
	fanInfo.pInputAssemblyState = &fanAssembly;
	fanInfo.basePipelineIndex = 0;

	constexpr auto cacheName = "grapihcsPipelineCache.bin";

	vpp::PipelineCache cache;
	if(vpp::fileExists(cacheName)) cache = {device(), cacheName};
	else cache = {device()};
	auto pipelines = vk::createGraphicsPipelines(device(), cache,
		{pipelineInfo, stripInfo, fanInfo});

	listPipeline_ = {device(), pipelines[0]};
	stripPipeline_ = {device(), pipelines[1]};
	fanPipeline_ = {device(), pipelines[2]};

	// save the cache to the file we tried to load it from
	vpp::save(cache, cacheName);

	// create a dummy image used for unbound image descriptors
	// TODO: find out if this is actually needed or a bug in the layers
	dummyTexture_ = {device(), (unsigned int) -1, {2, 2}, vk::Format::r8g8b8a8Unorm};
}

unsigned int Renderer::createTexture(vk::Format format, unsigned int w, unsigned int h,
	const std::uint8_t* data)
{
	++texID_;
	textures_.emplace_back(device(), texID_, vk::Extent2D{w, h}, format, data);
	return texID_;
}

bool Renderer::deleteTexture(unsigned int id)
{
	auto it = std::find_if(textures_.begin(), textures_.end(),
		[=](const auto& tex) { return tex.id() == id; });

	if(it == textures_.end()) return false;

	textures_.erase(it);
	return true;
}

void Renderer::start(unsigned int width, unsigned int height)
{
	// store (and set) viewport in some way
	width_ = width;
	height_ = height;

	vertices_.clear();
	drawDatas_.clear();
}

void Renderer::cancel()
{
}

void Renderer::flush()
{
	if(drawDatas_.empty())
		return;

	// allocate buffers
	auto uniformSize = sizeof(UniformData) * drawDatas_.size();
	auto bits = device().memoryTypeBits(vk::MemoryPropertyBits::hostVisible);

	if(uniformBuffer_.memorySize() < uniformSize) {
		vk::BufferCreateInfo bufInfo;
		bufInfo.usage = vk::BufferUsageBits::uniformBuffer;
		bufInfo.size = uniformSize;
		uniformBuffer_ = {device(), bufInfo, bits};
	}

	auto vertexSize = vertices_.size() * sizeof(NVGvertex);
	if(vertexBuffer_.memorySize() < vertexSize) {
		vk::BufferCreateInfo bufInfo;
		bufInfo.usage = vk::BufferUsageBits::vertexBuffer;
		bufInfo.size = vertexSize;
		vertexBuffer_ = {device(), bufInfo, bits};
	}

	// descriptorPool
	if(drawDatas_.size() > descriptorPoolSize_) {
		vk::DescriptorPoolSize typeCounts[2];
		typeCounts[0].type = vk::DescriptorType::uniformBuffer;
		typeCounts[0].descriptorCount = drawDatas_.size();

		typeCounts[1].type = vk::DescriptorType::combinedImageSampler;
		typeCounts[1].descriptorCount = drawDatas_.size();

		vk::DescriptorPoolCreateInfo poolInfo;
		poolInfo.poolSizeCount = 2;
		poolInfo.pPoolSizes = typeCounts;
		poolInfo.maxSets = drawDatas_.size();

		descriptorPool_ = {device(), poolInfo};
		descriptorPoolSize_ = drawDatas_.size();
	} else if(descriptorPool_) {
		vk::resetDescriptorPool(device(), descriptorPool_, {});
	}

	// update
	vpp::BufferUpdate update(uniformBuffer_, vpp::BufferLayout::std140);
	for(auto& data : drawDatas_) {
		update.alignUniform();

		auto offset = update.offset();
		update.add(vpp::raw(data.uniformData, 1)); // TODO

		data.descriptorSet = {descriptorLayout_, descriptorPool_};

		vpp::DescriptorSetUpdate descUpdate(data.descriptorSet);
		descUpdate.uniform({{uniformBuffer_, offset, sizeof(UniformData)}});

		vk::ImageView iv = dummyTexture_.viewableImage().vkImageView();
		dlg_assert(iv);
		if(data.texture != 0)
			iv = texture(data.texture)->viewableImage().vkImageView();

		auto layout = vk::ImageLayout::general; //XXX
		descUpdate.imageSampler({{{}, iv, layout}});

		descUpdate.apply();
	}

	update.apply()->finish();

	//vertex
	vpp::BufferUpdate vupdate(vertexBuffer_, vpp::BufferLayout::std140);
	vupdate.add(vpp::raw(*vertices_.data(), vertices_.size()));
	vupdate.apply()->finish();

	//render
	if(swapchain_) {
		renderer_.renderBlock(*presentQueue_);
	} else {
		vk::beginCommandBuffer(commandBuffer_, {});

		vk::ClearValue clearValues[2] {};
		clearValues[0].color = {0.f, 0.f, 0.f, 1.0f};
		clearValues[1].depthStencil = {1.f, 0};

		auto size = framebuffer_->size();

		vk::RenderPassBeginInfo beginInfo;
		beginInfo.renderPass = vkRenderPass();
		beginInfo.renderArea = {{0, 0}, {size.width, size.height}};
		beginInfo.clearValueCount = 2;
		beginInfo.pClearValues = clearValues;
		beginInfo.framebuffer = *framebuffer_;
		vk::cmdBeginRenderPass(commandBuffer_, beginInfo, vk::SubpassContents::eInline);

		vk::Viewport viewport;
		viewport.width = size.width;
		viewport.height = size.height;
		viewport.minDepth = 0.f;
		viewport.maxDepth = 1.f;
		vk::cmdSetViewport(commandBuffer_, 0, 1, viewport);

		//Update dynamic scissor state
		vk::Rect2D scissor;
		scissor.extent = {size.width, size.height};
		scissor.offset = {0, 0};
		vk::cmdSetScissor(commandBuffer_, 0, 1, scissor);

		record(commandBuffer_);

		vk::cmdEndRenderPass(commandBuffer_);
		vk::endCommandBuffer(commandBuffer_);

		vpp::CommandExecutionState state;
		device().submitManager().add(*renderQueue_, {commandBuffer_}, &state);
		state.wait();
	}

	//cleanup
	vertices_.clear();
	drawDatas_.clear();
}

void Renderer::fill(const NVGpaint& paint, const NVGscissor& scissor, float fringe,
	const float* bounds, nytl::Span<const NVGpath> paths)
{
	unused(bounds);
	auto& drawData = parsePaint(paint, scissor, fringe, fringe);
	drawData.paths.reserve(paths.size());

	for(auto& path : paths)
	{
		drawData.paths.emplace_back();
		drawData.paths.back().fillOffset = vertices_.size();
		drawData.paths.back().fillCount = path.nfill;
		vertices_.insert(vertices_.end(), path.fill, path.fill + path.nfill);

		if(edgeAA_ && path.nstroke > 0)
		{
			drawData.paths.back().strokeOffset = vertices_.size();
			drawData.paths.back().strokeCount = path.nstroke;
			vertices_.insert(vertices_.end(), path.stroke, path.stroke + path.nstroke);
		}
	}
}
void Renderer::stroke(const NVGpaint& paint, const NVGscissor& scissor, float fringe,
	float strokeWidth, nytl::Span<const NVGpath> paths)
{
	auto& drawData = parsePaint(paint, scissor, fringe, strokeWidth);
	drawData.paths.reserve(paths.size());

	for(auto& path : paths)
	{
		drawData.paths.emplace_back();
		drawData.paths.back().strokeOffset = vertices_.size();
		drawData.paths.back().strokeCount = path.nstroke;
		vertices_.insert(vertices_.end(), path.stroke, path.stroke + path.nstroke);
	}
}
void Renderer::triangles(const NVGpaint& paint, const NVGscissor& scissor,
	nytl::Span<const NVGvertex> verts)
{
	auto& drawData = parsePaint(paint, scissor, 1.f, 1.f);

	drawData.triangleOffset = vertices_.size();
	drawData.triangleCount = verts.size();
	vertices_.insert(vertices_.end(), verts.begin(), verts.end());
}

DrawData& Renderer::parsePaint(const NVGpaint& paint, const NVGscissor& scissor, float fringe,
	float strokeWidth)
{
	static constexpr auto typeColor = 1;
	static constexpr auto typeGradient = 2;
	static constexpr auto typeTexture = 3;

	static constexpr auto texTypeRGBA = 1;
	static constexpr auto texTypeA = 2;

	//update image
	drawDatas_.emplace_back();

	auto& data = drawDatas_.back();
	data.uniformData.viewSize = {float(width_), float(height_)};

	if(paint.image) {
		auto* tex = texture(paint.image);

		auto formatID = (tex->format() == vk::Format::r8g8b8a8Unorm) ? texTypeRGBA : texTypeA;
		data.uniformData.type = typeTexture;
		data.uniformData.texType = formatID;

		data.texture = paint.image;
	} else if(std::memcmp(&paint.innerColor, &paint.outerColor, sizeof(paint.innerColor)) == 0) {
		data.uniformData.type = typeColor;
		data.uniformData.texType = 0u;
	} else {
		data.uniformData.type = typeGradient;
		data.uniformData.texType = 0u;
	}

	//colors
	std::memcpy(&data.uniformData.innerColor, &paint.innerColor, sizeof(float) * 4);
	std::memcpy(&data.uniformData.outerColor, &paint.outerColor, sizeof(float) * 4);

	//mats
	float invxform[6];

	//scissor
	float scissorMat[4][4] {};
	if (scissor.extent[0] < -0.5f || scissor.extent[1] < -0.5f) {
		scissorMat[3][0] = 1.0f;
		scissorMat[3][1] = 1.0f;
		scissorMat[3][2] = 1.0f;
		scissorMat[3][3] = 1.0f;
	} else {
		nvgTransformInverse(invxform, scissor.xform);

		scissorMat[0][0] = invxform[0];
		scissorMat[0][1] = invxform[1];
		scissorMat[1][0] = invxform[2];
		scissorMat[1][1] = invxform[3];
		scissorMat[2][0] = invxform[4];
		scissorMat[2][1] = invxform[5];
		scissorMat[2][2] = 1.0f;

		//extent
		scissorMat[3][0] = scissor.extent[0];
		scissorMat[3][1] = scissor.extent[1];

		//scale
		scissorMat[3][2] = std::sqrt(scissor.xform[0]*scissor.xform[0] + scissor.xform[2]*
			scissor.xform[2]) / fringe;
		scissorMat[3][3] = std::sqrt(scissor.xform[1]*scissor.xform[1] + scissor.xform[3]*
			scissor.xform[3]) / fringe;
	}

	scissorMat[0][3] = paint.radius;
	scissorMat[1][3] = paint.feather;
	scissorMat[2][3] = strokeWidth;

	std::memcpy(&data.uniformData.scissorMat, &scissorMat, sizeof(scissorMat));

	//paint
	float paintMat[4][4] {};
	nvgTransformInverse(invxform, paint.xform);
	paintMat[0][0] = invxform[0];
	paintMat[0][1] = invxform[1];
	paintMat[1][0] = invxform[2];
	paintMat[1][1] = invxform[3];
	paintMat[2][0] = invxform[4];
	paintMat[2][1] = invxform[5];
	paintMat[2][2] = 1.0f;

	paintMat[3][0] = paint.extent[0];
	paintMat[3][1] = paint.extent[1];

	//strokeMult
	paintMat[0][3] = (strokeWidth * 0.5f + fringe * 0.5f) / fringe;

	std::memcpy(&data.uniformData.paintMat, &paintMat, sizeof(paintMat));
	return data;
}

const Texture* Renderer::texture(unsigned int id) const
{
	auto it = std::find_if(textures_.begin(), textures_.end(),
		[=](const auto& tex) { return tex.id() == id; });

	if(it == textures_.end()) return nullptr;
	return &(*it);
}

Texture* Renderer::texture(unsigned int id)
{
	auto it = std::find_if(textures_.begin(), textures_.end(),
		[=](const auto& tex) { return tex.id() == id; });

	if(it == textures_.end()) return nullptr;
	return &(*it);
}

void Renderer::record(vk::CommandBuffer cmdBuffer)
{
	int bound = 0;
	vk::cmdBindVertexBuffers(cmdBuffer, 0, {vertexBuffer_}, {0});

	for(auto& data : drawDatas_)
	{
		vk::cmdBindDescriptorSets(cmdBuffer, vk::PipelineBindPoint::graphics, pipelineLayout_,
			0, {data.descriptorSet}, {});

		for(auto& path : data.paths) {
			if(path.fillCount > 0) {
				if(bound != 1) {
					vk::cmdBindPipeline(cmdBuffer, vk::PipelineBindPoint::graphics, fanPipeline_);
					bound = 1;
				}

				vk::cmdDraw(cmdBuffer, path.fillCount, 1, path.fillOffset, 0);
			} if(path.strokeCount > 0) {
				if(bound != 2) {
					vk::cmdBindPipeline(cmdBuffer, vk::PipelineBindPoint::graphics, stripPipeline_);
					bound = 2;
				}

				vk::cmdDraw(cmdBuffer, path.strokeCount, 1, path.strokeOffset, 0);
			}
		}

		if(data.triangleCount > 0) {
			if(bound != 3) {
				vk::cmdBindPipeline(cmdBuffer, vk::PipelineBindPoint::graphics, listPipeline_);
				bound = 3;
			}

			vk::cmdDraw(cmdBuffer, data.triangleCount, 1, data.triangleOffset, 0);
		}
	}
}

void Renderer::initRenderPass(const vpp::Device& dev, vk::Format attachment)
{
	vk::AttachmentDescription attachments[2] {};

	//color from swapchain
	attachments[0].format = attachment;
	attachments[0].samples = vk::SampleCountBits::e1;
	attachments[0].loadOp = vk::AttachmentLoadOp::clear;
	attachments[0].storeOp = vk::AttachmentStoreOp::store;
	attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[0].initialLayout = vk::ImageLayout::undefined;
	attachments[0].finalLayout = vk::ImageLayout::presentSrcKHR;

	vk::AttachmentReference colorReference;
	colorReference.attachment = 0;
	colorReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	//stencil attachment
	//will not be used as depth buffer
	attachments[1].format = vk::Format::s8Uint;
	attachments[1].samples = vk::SampleCountBits::e1;
	attachments[1].loadOp = vk::AttachmentLoadOp::clear;
	attachments[1].storeOp = vk::AttachmentStoreOp::store;
	attachments[1].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[1].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[1].initialLayout = vk::ImageLayout::undefined;
	attachments[1].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;
	// attachments[1].initialLayout = vk::ImageLayout::depthStencilAttachmentOptimal;
	// attachments[1].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::AttachmentReference depthReference;
	depthReference.attachment = 1;
	depthReference.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	//only subpass
	vk::SubpassDescription subpass;
	subpass.pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpass.flags = {};
	subpass.inputAttachmentCount = 0;
	subpass.pInputAttachments = nullptr;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	subpass.pResolveAttachments = nullptr;
	subpass.pDepthStencilAttachment = &depthReference;
	subpass.preserveAttachmentCount = 0;
	subpass.pPreserveAttachments = nullptr;

	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = 2;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = 0;
	renderPassInfo.pDependencies = nullptr;

	renderPass_ = {dev, renderPassInfo};
}


//Texture
Texture::Texture(const vpp::Device& dev, unsigned int xid, const vk::Extent2D& size,
	vk::Format format, const std::uint8_t* data)
		: format_(format), id_(xid), width_(size.width), height_(size.height)
{
	vk::Extent3D extent {width(), height(), 1};

	auto info = vpp::ViewableImage::defaultColor2D();
	info.imgInfo.extent = extent;
	info.imgInfo.initialLayout = vk::ImageLayout::undefined;
	info.imgInfo.tiling = vk::ImageTiling::linear;

	info.imgInfo.format = format;
	info.viewInfo.format = format;

	info.imgInfo.usage = vk::ImageUsageBits::sampled;
	info.memoryTypeBits = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);

	// info.imgInfo.usage = vk::ImageUsageBits::transferDst | vk::ImageUsageBits::sampled;
	viewableImage_ = {dev, info};

	vpp::changeLayout(viewableImage_.image(), vk::ImageLayout::undefined,
		vk::ImageLayout::general, {vk::ImageAspectBits::color, 0, 1, 0, 1})->finish();

	vk::ImageLayout layout = vk::ImageLayout::general;
	if(data)
		vpp::fill(viewableImage_.image(), *data, format, layout, extent,
			{vk::ImageAspectBits::color, 0, 1})->finish();
}

void Texture::update(const vk::Offset2D& offset, const vk::Extent2D& extent,
	const std::uint8_t& data)
{
	unused(offset, extent);

	//TODO: really only update the given offset/extent
	//would need an extra data copy
	//or modify vpp::fill(Image) to also accept non tightly packed data
	//or just fill it manually...
	// vk::Offset3D ioffset {offset.x, offset.y, 0};

	vk::Extent3D iextent {width(), height(), 1};
	vk::ImageLayout layout = vk::ImageLayout::general;
	fill(viewableImage_.image(), data, format(), layout, iextent,
		{vk::ImageAspectBits::color, 0, 0})->finish();
}


//RenderImpl
void RenderImpl::build(unsigned int, const vpp::RenderPassInstance& ini)
{
	renderer->record(ini.vkCommandBuffer());
}

std::vector<vk::ClearValue> RenderImpl::clearValues(unsigned int)
{
	// TODO
	std::vector<vk::ClearValue> ret(1, vk::ClearValue{});
	ret[0].color = {0.f, 0.f, 0.f, 1.0f};
	// ret[0].depthStencil = {1.f, 0};
	return ret;
}

void RenderImpl::frame(unsigned int id)
{
	swapchainRenderer->record(id);
}

//class that derives vvg::Renderer for the C implementation.
using NonOwnedDevicePtr = std::unique_ptr<vpp::NonOwned<vpp::Device>>;
using NonOwnedSwapchainPtr = std::unique_ptr<vpp::NonOwned<vpp::Swapchain>>;

class RendererCImpl : public Renderer {
public:
	RendererCImpl(NonOwnedDevicePtr dev, NonOwnedSwapchainPtr swapchain)
		: Renderer(*swapchain), dev_(std::move(dev)), swapchain_(std::move(swapchain)) {}

	virtual ~RendererCImpl()
	{
		//first destruct the Renderer since it may depend on the device and swapchain
		Renderer::operator=({});
	}

protected:
	NonOwnedDevicePtr dev_;
	NonOwnedSwapchainPtr swapchain_;
};

} // namespace vvg

// The NVGcontext backend implementation which just calls the Renderer/Texture member functions
namespace {

vvg::Renderer& resolve(void* ptr)
{
	return *static_cast<vvg::Renderer*>(ptr);
}

int renderCreate(void* uptr)
{
	vvg::unused(uptr);
	return 1;
}

int createTexture(void* uptr, int type, int w, int h, int imageFlags, const unsigned char* data)
{
	vvg::unused(uptr, type, imageFlags);
	auto& renderer = resolve(uptr);
	auto format = (type == NVG_TEXTURE_ALPHA) ? vk::Format::r8Unorm : vk::Format::r8g8b8a8Unorm;
	return renderer.createTexture(format, w, h, data);
}
int deleteTexture(void* uptr, int image)
{
	auto& renderer = resolve(uptr);
	return renderer.deleteTexture(image);
}
int updateTexture(void* uptr, int image, int x, int y, int w, int h, const unsigned char* data)
{
	auto& renderer = resolve(uptr);
	auto* tex = renderer.texture(image);
	if(!tex) return 0;

	vk::Extent2D extent {(unsigned int) w, (unsigned int) h};
	vk::Offset2D offset{x, y};
	tex->update(offset, extent, *data);

	return 1;
}
int getTextureSize(void* uptr, int image, int* w, int* h)
{
	auto& renderer = resolve(uptr);
	auto* tex = renderer.texture(image);
	if(!tex) return 0;

	//TODO: first check pointers?
	*w = tex->width();
	*h = tex->width();
	return 1;
}
void viewport(void* uptr, int width, int height)
{
	auto& renderer = resolve(uptr);
	renderer.start(width, height);
}
void cancel(void* uptr)
{
	auto& renderer = resolve(uptr);
	renderer.cancel();
}
void flush(void* uptr)
{
	auto& renderer = resolve(uptr);
	renderer.flush();
}
void fill(void* uptr, NVGpaint* paint, NVGscissor* scissor, float fringe, const float* bounds,
	const NVGpath* paths, int npaths)
{
	auto& renderer = resolve(uptr);
	renderer.fill(*paint, *scissor, fringe, bounds, {paths, std::size_t(npaths)});
}
void stroke(void* uptr, NVGpaint* paint, NVGscissor* scissor, float fringe, float strokeWidth,
	const NVGpath* paths, int npaths)
{
	auto& renderer = resolve(uptr);
	renderer.stroke(*paint, *scissor, fringe, strokeWidth, {paths, std::size_t(npaths)});
}
void triangles(void* uptr, NVGpaint* paint, NVGscissor* scissor, const NVGvertex* verts, int nverts)
{
	auto& renderer = resolve(uptr);
	renderer.triangles(*paint, *scissor, {verts, std::size_t(nverts)});
}
void renderDelete(void* uptr)
{
	auto& renderer = resolve(uptr);
	delete &renderer;
}

const NVGparams nvgContextImpl =
{
	nullptr,
	1,
	renderCreate,
	createTexture,
	deleteTexture,
	updateTexture,
	getTextureSize,
	viewport,
	cancel,
	flush,
	fill,
	stroke,
	triangles,
	renderDelete
};

} // anonymous util namespace

// implementation of the C++ create api
namespace vvg {

NVGcontext* createContext(std::unique_ptr<Renderer> renderer)
{
	auto impl = nvgContextImpl;
	auto rendererPtr = renderer.get();
	impl.userPtr = renderer.release();
	auto ret = nvgCreateInternal(&impl);
	if(!ret) delete rendererPtr;
	return ret;
}

NVGcontext* createContext(const vpp::Swapchain& swapchain)
{
	return createContext(std::make_unique<Renderer>(swapchain));
}

NVGcontext* createContext(const vpp::Framebuffer& fb, vk::RenderPass rp)
{
	return createContext(std::make_unique<Renderer>(fb, rp));
}

void destroyContext(const NVGcontext& context)
{
	auto ctx = const_cast<NVGcontext*>(&context);
	nvgDeleteInternal(ctx);
}

const Renderer& getRenderer(const NVGcontext& context)
{
	auto ctx = const_cast<NVGcontext*>(&context);
	return resolve(nvgInternalParams(ctx)->userPtr);
}
Renderer& getRenderer(NVGcontext& context)
{
	return resolve(nvgInternalParams(&context)->userPtr);
}

}

// implementation of the C api
NVGcontext* vvgCreate(const VVGContextDescription* descr)
{
	auto vkInstance = (vk::Instance)descr->instance;
	auto vkPhDev = (vk::PhysicalDevice)descr->phDev;
	auto vkDev = (vk::Device)descr->device;
	auto vkQueue = (vk::Queue)descr->queue;
	auto vkSwapchain = (vk::SwapchainKHR)descr->swapchain;
	auto vkFormat = (vk::Format)descr->swapchainFormat;
	auto vkExtent = (const vk::Extent2D&)descr->swapchainSize;

	vvg::NonOwnedDevicePtr dev(new vpp::NonOwned<vpp::Device>(vkInstance,
		vkPhDev, vkDev, {{vkQueue, descr->queueFamily}}));

	// NOTE that this constructs the swapchain with an invalid surface parameter and it
	// can therefore not be reiszed.
	vvg::NonOwnedSwapchainPtr swapchain(new vpp::NonOwned<vpp::Swapchain>(*dev,
		vkSwapchain, {}, vkExtent, vkFormat));

	auto renderer = std::make_unique<vvg::RendererCImpl>(std::move(dev), std::move(swapchain));
	return vvg::createContext(std::move(renderer));
}

void vvgDestroy(const NVGcontext* context)
{
	auto ctx = const_cast<NVGcontext*>(context);
	nvgDeleteInternal(ctx);
}
