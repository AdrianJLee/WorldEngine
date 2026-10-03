#include "Renderer3D_Internal.h"

namespace World
{

using namespace Renderer3DDetail;


void Renderer3D::ApplyShadowCaster(LightRig& rig, const glm::mat4& lightViewProjection){
		rig.Uniforms.ShadowViewProjection = lightViewProjection;
		rig.Uniforms.ShadowParams.x = 1.0f;
	}


void Renderer3D::ReportLighting(const LightRig& rig, double shadowPassMilliseconds){
		State& state = GetState();
		state.Stats.Lights = rig.TotalLights;
		state.Stats.MaxLights = GetMaxDirectionalLights() + GetMaxPointLights();
		state.Stats.DroppedLights = rig.DroppedLights;
		state.Stats.ShadowPassMilliseconds = shadowPassMilliseconds;

		// 首帧或"数量/截断/阴影开关"变化时打一行(自动化断言用;逐帧刷屏没有信息量)。
		const uint32_t directional = rig.Uniforms.LightCounts.x;
		const uint32_t point = rig.Uniforms.LightCounts.y;
		const int32_t shadow = rig.Uniforms.ShadowParams.x > 0.5f ? 1 : 0;
		if (directional == state.LastLoggedDirectional && point == state.LastLoggedPoint &&
			rig.DroppedLights == state.LastLoggedDropped && shadow == state.LastLoggedShadow)
			return;
		state.LastLoggedDirectional = directional;
		state.LastLoggedPoint = point;
		state.LastLoggedDropped = rig.DroppedLights;
		state.LastLoggedShadow = shadow;
		WLD_CORE_INFO("[lighting] directional={0} point={1} dropped={2} shadowMs={3:.3f} shadow={4}",
			directional, point, rig.DroppedLights, shadowPassMilliseconds, shadow);
	}


std::vector<Rhi::DescriptorWrite> Renderer3D::MakeGlobalLightingWrites( const Rhi::Handle<Rhi::Buffer>& lightUniformBuffer){
		State& state = GetState();
		std::vector<Rhi::DescriptorWrite> writes;
		const Rhi::Handle<Rhi::Buffer> buffer = lightUniformBuffer ? lightUniformBuffer : state.DefaultLightBuffer;
		if (buffer)
		{
			Rhi::DescriptorWrite light;
			light.Binding = 2;
			light.Type = Rhi::DescriptorType::UniformBuffer;
			light.Buffer = buffer;
			writes.push_back(light);
		}
		if (state.ShadowMapTexture)
		{
			Rhi::DescriptorWrite shadow;
			shadow.Binding = 3;
			shadow.Type = Rhi::DescriptorType::CombinedImageSampler;
			shadow.Texture = state.ShadowMapTexture;
			shadow.Sampler = state.ShadowSampler;
			writes.push_back(shadow);
		}
		return writes;
	}


Rhi::Handle<Rhi::RenderPass> Renderer3D::GetShadowRenderPass(){
		return GetState().ShadowPass;
	}


Rhi::Handle<Rhi::Framebuffer> Renderer3D::GetShadowFramebuffer(){
		return GetState().ShadowFramebuffer;
	}


Rhi::Handle<Rhi::Texture> Renderer3D::GetShadowMapTexture(){
		return GetState().ShadowMapTexture;
	}


Rhi::Handle<Rhi::Sampler> Renderer3D::GetShadowMapSampler(){
		return GetState().ShadowSampler;
	}


void Renderer3D::BeginShadowPass(const Rhi::Handle<Rhi::CommandBuffer>& commandBuffer){
		State& state = GetState();
		state.CommandBuffer = commandBuffer;
		state.ShadowObjectIndex = 0;
		// D5c-3b:阴影通道与主通道共用蒙皮调色板池,游标同样从 0 起(两者同属一次提交)。
		state.PaletteCursor = 0;
		if (!commandBuffer)
			return;
		// 阴影贴图是固定尺寸的离屏目标,视口/裁剪按贴图边长设置(管线是动态视口状态)。
		commandBuffer->SetViewport({ 0.0f, 0.0f, static_cast<float>(state.ShadowMapSize),
			static_cast<float>(state.ShadowMapSize) });
		commandBuffer->SetScissor({ 0, 0, state.ShadowMapSize, state.ShadowMapSize });
	}


uint32_t Renderer3D::SubmitShadow(const Ref<Mesh>& mesh, const glm::mat4& transform){
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.ShadowPipeline)
			return UINT32_MAX;
		if (state.ShadowObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ShadowObjectIndex++;

		// 阴影只需要 u_Model(顶点按 u_ShadowViewProjection × u_Model 变换),其余字段留默认。
		ObjectUniforms uniforms;
		uniforms.Model = transform;
		WriteObjectUniforms(state, slot, index, uniforms,
			state.ShadowUniformBuffers[slot][index], state.ShadowObjectSets[slot][index]);

		state.CommandBuffer->BindPipeline(state.ShadowPipeline);
		state.CommandBuffer->BindDescriptorSet(state.ShadowObjectSets[slot][index], 1);
		state.CommandBuffer->BindVertexBuffer(0, cached->second.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(cached->second.IndexBuffer);
		state.CommandBuffer->DrawIndexed(cached->second.IndexCount);
		return index;
	}


uint32_t Renderer3D::SubmitShadowSubmesh(const Ref<Mesh>& mesh, uint32_t submeshIndex, const glm::mat4& transform){
		State& state = GetState();
		if (!mesh || !state.CommandBuffer || !state.ShadowPipeline)
			return UINT32_MAX;
		if (submeshIndex >= mesh->GetSubmeshes().size())
			return UINT32_MAX;
		if (state.ShadowObjectIndex >= kObjectsPerFrame)
			return UINT32_MAX;
		const MeshSubmesh& submesh = mesh->GetSubmeshes()[submeshIndex];
		if (submesh.IndexCount == 0)
			return UINT32_MAX;

		EnsureMeshBuffers(mesh);
		const auto cached = state.MeshCache.find(mesh.get());
		if (cached == state.MeshCache.end() || !cached->second.VertexBuffer)
			return UINT32_MAX;

		const uint32_t slot = Renderer::FrameSlot() % Renderer::FramesInFlight;
		const uint32_t index = state.ShadowObjectIndex++;

		// 阴影只需要 u_Model(顶点按 u_ShadowViewProjection × u_Model 变换),其余字段留默认。
		ObjectUniforms uniforms;
		uniforms.Model = transform;
		WriteObjectUniforms(state, slot, index, uniforms,
			state.ShadowUniformBuffers[slot][index], state.ShadowObjectSets[slot][index]);

		state.CommandBuffer->BindPipeline(state.ShadowPipeline);
		state.CommandBuffer->BindDescriptorSet(state.ShadowObjectSets[slot][index], 1);
		state.CommandBuffer->BindVertexBuffer(0, cached->second.VertexBuffer);
		state.CommandBuffer->BindIndexBuffer(cached->second.IndexBuffer);
		state.CommandBuffer->DrawIndexed(submesh.IndexCount, 1, submesh.IndexOffset);
		return index;
	}


void Renderer3D::EndShadowPass(){
		State& state = GetState();
		state.CommandBuffer = nullptr;
		state.ShadowObjectIndex = 0;
	}


Renderer3D::Statistics Renderer3D::GetStats(){
		return GetState().Stats;
	}


void Renderer3D::ResetStats(){
		GetState().Stats = {};
	}


void Renderer3D::ReportSceneStatistics(const SceneStatistics& statistics){
		GetState().SceneStats = statistics;
	}


Renderer3D::SceneStatistics Renderer3D::GetSceneStatistics(){
		return GetState().SceneStats;
	}


uint32_t Renderer3D::GetObjectsPerFrameLimit(){
		return kObjectsPerFrame;
	}


uint32_t Renderer3D::GetShadowMapSize(){
		// Init 之前/未初始化时退回默认值(测试直接调 BuildLightRig 也走这条)。
		return GetState().ShadowMapSize;
	}


uint32_t Renderer3D::GetMaxDirectionalLights(){
		return std::min(RenderSettings::Get().MaxDirectionalLights, MaxDirectionalLightCapacity);
	}


uint32_t Renderer3D::GetMaxPointLights(){
		return std::min(RenderSettings::Get().MaxPointLights, MaxPointLightCapacity);
	}

}
