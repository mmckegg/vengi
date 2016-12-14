#include "RawVolumeRenderer.h"
#include "voxel/polyvox/CubicSurfaceExtractor.h"
#include "voxel/MaterialColor.h"
#include "video/ScopedLineWidth.h"
#include "video/ScopedPolygonMode.h"

namespace frontend {

const std::string MaxDepthBufferUniformName = "u_cascades";

RawVolumeRenderer::RawVolumeRenderer(bool renderAABB, bool renderWireframe, bool renderGrid) :
		_rawVolume(nullptr), _mesh(nullptr),
		_worldShader(shader::WorldShader::getInstance()), _renderAABB(renderAABB),
		_renderGrid(renderGrid), _renderWireframe(renderWireframe) {
	_sunDirection = glm::vec3(glm::left.x, glm::down.y, 0.0f);
}

bool RawVolumeRenderer::init() {
	if (!_worldShader.setup()) {
		Log::error("Failed to initialize the color shader");
		return false;
	}

	const int shaderMaterialColorsArraySize = _worldShader.getUniformArraySize("u_materialcolor");
	const int materialColorsArraySize = voxel::getMaterialColors().size();
	if (shaderMaterialColorsArraySize != materialColorsArraySize) {
		Log::error("Shader parameters and material colors don't match in their size: %i - %i",
				shaderMaterialColorsArraySize, materialColorsArraySize);
		return false;
	}

	if (!_shapeRenderer.init()) {
		Log::error("Failed to initialize the shape renderer");
		return false;
	}

	_vertexBufferIndex = _vertexBuffer.create();
	if (_vertexBufferIndex == -1) {
		Log::error("Could not create the vertex buffer object");
		return false;
	}

	_indexBufferIndex = _vertexBuffer.create(nullptr, 0, GL_ELEMENT_ARRAY_BUFFER);
	if (_indexBufferIndex == -1) {
		Log::error("Could not create the vertex buffer object for the indices");
		return false;
	}

	const voxel::MaterialColorArray& materialColors = voxel::getMaterialColors();
	_materialBuffer.create(materialColors.size() * sizeof(voxel::MaterialColorArray::value_type), &materialColors.front());

	video::VertexBuffer::Attribute attributePos;
	attributePos.bufferIndex = _vertexBufferIndex;
	attributePos.index = _worldShader.getLocationPos();
	attributePos.stride = sizeof(voxel::VoxelVertex);
	attributePos.size = _worldShader.getComponentsPos();
	attributePos.type = GL_UNSIGNED_BYTE;
	attributePos.typeIsInt = true;
	attributePos.offset = offsetof(voxel::VoxelVertex, position);
	_vertexBuffer.addAttribute(attributePos);

	video::VertexBuffer::Attribute attributeInfo;
	attributeInfo.bufferIndex = _vertexBufferIndex;
	attributeInfo.index = _worldShader.getLocationInfo();
	attributeInfo.stride = sizeof(voxel::VoxelVertex);
	attributeInfo.size = _worldShader.getComponentsInfo();
	attributeInfo.type = GL_UNSIGNED_BYTE;
	attributeInfo.typeIsInt = true;
	attributeInfo.offset = offsetof(voxel::VoxelVertex, ambientOcclusion);
	_vertexBuffer.addAttribute(attributeInfo);

	_whiteTexture = video::createWhiteTexture("**whitetexture**");

	_mesh = new voxel::Mesh(128, 128, true);

	return true;
}

bool RawVolumeRenderer::onResize(const glm::ivec2& position, const glm::ivec2& dimension) {
	core_trace_scoped(RawVolumeRendererOnResize);
	_sunLight.init(_sunDirection, position, dimension);

	const int maxDepthBuffers = _worldShader.getUniformArraySize(MaxDepthBufferUniformName);
	_depthBuffer.shutdown();
	const glm::ivec2 smSize(core::Var::get(cfg::ClientShadowMapSize, "2048")->intVal());
	if (!_depthBuffer.init(smSize, video::DepthBufferMode::DEPTH_CMP, maxDepthBuffers)) {
		return false;
	}
	return true;
}

bool RawVolumeRenderer::update(const std::vector<voxel::VoxelVertex>& vertices, const std::vector<voxel::IndexType>& indices) {
	core_trace_scoped(RawVolumeRendererUpdate);
	if (!_vertexBuffer.update(_vertexBufferIndex, vertices)) {
		Log::error("Failed to update the vertex buffer");
		return false;
	}
	if (!_vertexBuffer.update(_indexBufferIndex, indices)) {
		Log::error("Failed to update the index buffer");
		return false;
	}
	return true;
}

bool RawVolumeRenderer::extract() {
	core_trace_scoped(RawVolumeRendererExtract);
	if (_rawVolume == nullptr) {
		return false;
	}

	if (_mesh == nullptr) {
		return false;
	}

	/// implementation of a function object for deciding when
	/// the cubic surface extractor should insert a face between two voxels.
	///
	/// The criteria used here are that the voxel in front of the potential
	/// quad should have a value of zero (which would typically indicate empty
	/// space) while the voxel behind the potential quad would have a value
	/// geater than zero (typically indicating it is solid).
	struct CustomIsQuadNeeded {
		inline bool operator()(const voxel::Voxel& back, const voxel::Voxel& front, voxel::Voxel& materialToUse, voxel::FaceNames face, int x, int z) const {
			if (isBlocked(back.getMaterial()) && !isBlocked(front.getMaterial())) {
				materialToUse = back;
				return true;
			}
			return false;
		}
	};

	voxel::Region r = _rawVolume->getRegion();
	r.shiftUpperCorner(1, 1, 1);
	voxel::extractCubicMesh(_rawVolume, r, _mesh, CustomIsQuadNeeded());
	const voxel::IndexType* meshIndices = _mesh->getRawIndexData();
	const voxel::VoxelVertex* meshVertices = _mesh->getRawVertexData();
	const size_t meshNumberIndices = _mesh->getNoOfIndices();
	if (meshNumberIndices == 0) {
		_vertexBuffer.update(_vertexBufferIndex, nullptr, 0);
		_vertexBuffer.update(_indexBufferIndex, nullptr, 0);
	} else {
		const size_t meshNumberVertices = _mesh->getNoOfVertices();
		if (!_vertexBuffer.update(_vertexBufferIndex, meshVertices, sizeof(voxel::VoxelVertex) * meshNumberVertices)) {
			Log::error("Failed to update the vertex buffer");
			return false;
		}
		if (!_vertexBuffer.update(_indexBufferIndex, meshIndices, sizeof(voxel::IndexType) * meshNumberIndices)) {
			Log::error("Failed to update the index buffer");
			return false;
		}
	}

	return true;
}

void RawVolumeRenderer::render(const video::Camera& camera) {
	core_trace_scoped(RawVolumeRendererRender);

	if (_renderGrid) {
		const voxel::Region& region = _rawVolume->getRegion();
		const glm::vec3& center = glm::vec3(region.getCentre());
		const glm::vec3& halfWidth = glm::vec3(region.getDimensionsInCells()) / 2.0f;
		const core::Plane planeLeft  (glm::left,     center + glm::vec3(-halfWidth.x, 0.0f, 0.0f));
		const core::Plane planeRight (glm::right,    center + glm::vec3( halfWidth.x, 0.0f, 0.0f));
		const core::Plane planeBottom(glm::down,     center + glm::vec3(0.0f, -halfWidth.y, 0.0f));
		const core::Plane planeTop   (glm::up,       center + glm::vec3(0.0f,  halfWidth.y, 0.0f));
		const core::Plane planeNear  (glm::forward,  center + glm::vec3(0.0f, 0.0f, -halfWidth.z));
		const core::Plane planeFar   (glm::backward, center + glm::vec3(0.0f, 0.0f,  halfWidth.z));

		if (planeFar.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexXYFar, camera);
		}
		if (planeNear.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexXYNear, camera);
		}

		if (planeBottom.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexXZNear, camera);
		}
		if (planeTop.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexXZFar, camera);
		}

		if (planeLeft.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexYZNear, camera);
		}
		if (planeRight.isBackSide(camera.position())) {
			_shapeRenderer.render(_gridMeshIndexYZFar, camera);
		}
	} else if (_renderAABB) {
		_shapeRenderer.render(_aabbMeshIndex, camera);
	}

	const GLuint nIndices = _vertexBuffer.elements(_indexBufferIndex, 1, sizeof(uint32_t));
	if (nIndices == 0) {
		return;
	}

	_sunLight.update(0.0f, camera);

	// Enable depth test
	glEnable(GL_DEPTH_TEST);
	// Accept fragment if it closer to the camera than the former one
	glDepthFunc(GL_LEQUAL);
	// Cull triangles whose normal is not towards the camera
	glEnable(GL_CULL_FACE);
	glDepthMask(GL_TRUE);

	_whiteTexture->bind(0);

	video::ScopedShader scoped(_worldShader);
	_worldShader.setModel(glm::mat4());
	_worldShader.setViewprojection(camera.viewProjectionMatrix());
	_worldShader.setUniformBuffer("u_materialblock", _materialBuffer);
	_worldShader.setTexture(0);
	_worldShader.setShadowmap(1);
	_worldShader.setFogrange(250.0f);
	_worldShader.setViewdistance(camera.farPlane());
	_worldShader.setLightdir(_sunLight.direction());
	_worldShader.setDepthsize(glm::vec2(_depthBuffer.dimension()));
	_worldShader.setDiffuseColor(_diffuseColor);
	_worldShader.setAmbientColor(_ambientColor);
	_worldShader.setFogcolor(glm::vec3(core::Color::LightBlue));
	_worldShader.setDebugColor(1.0f);
	int maxDepthBuffers = maxDepthBuffers = _worldShader.getUniformArraySize(MaxDepthBufferUniformName);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(std::enum_value(_depthBuffer.textureType()), _depthBuffer.texture());
	core_assert_always(_vertexBuffer.bind());
	static_assert(sizeof(voxel::IndexType) == sizeof(uint32_t), "Index type doesn't match");
	glDrawElements(GL_TRIANGLES, nIndices, GL_UNSIGNED_INT, nullptr);

	if (_renderWireframe && camera.polygonMode() == video::PolygonMode::Solid) {
		video::ScopedPolygonMode polygonMode(video::PolygonMode::WireFrame, glm::vec2(2.0f));
		video::ScopedLineWidth lineWidth(2.0f, true);
		shaderSetUniformIf(_worldShader, setUniformf, "u_debug_color", 0.0);
		glDrawElements(GL_TRIANGLES, nIndices, GL_UNSIGNED_INT, nullptr);
	}

	_vertexBuffer.unbind();

	_whiteTexture->unbind();

	for (int i = 0; i < maxDepthBuffers; ++i) {
		glActiveTexture(GL_TEXTURE1 + i);
		glBindTexture(GL_TEXTURE_2D, 0);
	}
	glActiveTexture(GL_TEXTURE0);

	GL_checkError();
}

voxel::RawVolume* RawVolumeRenderer::setVolume(voxel::RawVolume* volume) {
	core_trace_scoped(RawVolumeRendererSetVolume);
	voxel::RawVolume* old = _rawVolume;
	_rawVolume = volume;
	if (_rawVolume != nullptr) {
		const voxel::Region& region = _rawVolume->getRegion();
		const core::AABB<int>& intaabb = region.aabb();
		const core::AABB<float> aabb(glm::vec3(intaabb.getLowerCorner()), glm::vec3(intaabb.getUpperCorner()));
		_shapeBuilder.clear();
		_shapeBuilder.aabb(aabb, false);
		if (_aabbMeshIndex == -1) {
			_aabbMeshIndex = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_aabbMeshIndex, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridXY(aabb, false);
		if (_gridMeshIndexXYFar == -1) {
			_gridMeshIndexXYFar = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexXYFar, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridXZ(aabb, false);
		if (_gridMeshIndexXZFar == -1) {
			_gridMeshIndexXZFar = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexXZFar, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridYZ(aabb, false);
		if (_gridMeshIndexYZFar == -1) {
			_gridMeshIndexYZFar = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexYZFar, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridXY(aabb, true);
		if (_gridMeshIndexXYNear == -1) {
			_gridMeshIndexXYNear = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexXYNear, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridXZ(aabb, true);
		if (_gridMeshIndexXZNear == -1) {
			_gridMeshIndexXZNear = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexXZNear, _shapeBuilder);
		}

		_shapeBuilder.clear();
		_shapeBuilder.aabbGridYZ(aabb, true);
		if (_gridMeshIndexYZNear == -1) {
			_gridMeshIndexYZNear = _shapeRenderer.createMesh(_shapeBuilder);
		} else {
			_shapeRenderer.update(_gridMeshIndexYZNear, _shapeBuilder);
		}
	} else {
		_shapeBuilder.clear();
	}
	return old;
}

voxel::RawVolume* RawVolumeRenderer::shutdown() {
	_vertexBuffer.shutdown();
	_worldShader.shutdown();
	_vertexBufferIndex = -1;
	_indexBufferIndex = -1;
	_aabbMeshIndex = -1;
	_gridMeshIndexXYNear = -1;
	_gridMeshIndexXYFar = -1;
	_gridMeshIndexXZNear = -1;
	_gridMeshIndexXZFar = -1;
	_gridMeshIndexYZNear = -1;
	_gridMeshIndexYZFar = -1;
	if (_mesh != nullptr) {
		delete _mesh;
	}
	_mesh = nullptr;
	voxel::RawVolume* old = _rawVolume;
	if (_whiteTexture) {
		_whiteTexture->shutdown();
		_whiteTexture = video::TexturePtr();
	}
	_rawVolume = nullptr;
	_shapeRenderer.shutdown();
	_shapeBuilder.shutdown();
	_depthBuffer.shutdown();
	_materialBuffer.shutdown();
	return old;
}

size_t RawVolumeRenderer::numVertices() const {
	if (_mesh == nullptr) {
		return 0u;
	}
	return _mesh->getNoOfVertices();
}

const voxel::VoxelVertex* RawVolumeRenderer::vertices() const {
	if (_mesh == nullptr) {
		return 0u;
	}
	return _mesh->getRawVertexData();
}

size_t RawVolumeRenderer::numIndices() const {
	if (_mesh == nullptr) {
		return 0u;
	}
	return _mesh->getNoOfIndices();
}

const voxel::IndexType* RawVolumeRenderer::indices() const {
	if (_mesh == nullptr) {
		return 0u;
	}
	return _mesh->getRawIndexData();
}

}
