/**
 * @file
 */

#pragma once

#include "voxelformat/private/mesh/MeshFormat.h"
#include "core/collection/DynamicArray.h"
#include "core/collection/StringMap.h"
#include "image/Image.h"
#include "palette/PaletteLookup.h"

namespace voxelformat {

/**
 * @brief Quake bsp format
 *
 * @ingroup Formats
 *
 * https://www.gamers.org/dEngine/quake/spec/quake-spec34/qkspec_4.htm#CBSPF
 */
class QuakeBSPFormat : public MeshFormat {
private:
	// ----------------------------------------------
	// internal bsp (ufoai) structures
	struct BspLump {
		uint32_t offset = 0;
		uint32_t len = 0;
	};
	static_assert(sizeof(BspLump) == 8, "Unexpected size of BspLump");

	struct BspHeader {
		uint32_t magic = 0;
		uint32_t version = 0;
		BspLump lumps[30];
	};

	// q1
	struct BspTextureBase {
		float vecS[3];
		float distS;
		float vecT[3];
		float distT;
		uint32_t surfaceFlags = 0; // miptex index in q1
		uint32_t value = 0;
	};

	struct BspTexture : public BspTextureBase {
		char name[32] = "";
	};
	static_assert(sizeof(BspTexture) == 72, "Unexpected size of BspTexture");

	struct BspModel {
		float mins[3]{0.0f, 0.0f, 0.0f};
		float maxs[3]{0.0f, 0.0f, 0.0f};
		float position[3]{0.0f, 0.0f, 0.0f};
		int32_t node = 0;
		int32_t faceId = 0;
		int32_t faceCount = 0;
	};
	static_assert(sizeof(BspModel) == 48, "Unexpected size of BspModel");

	struct BspVertex {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};
	static_assert(sizeof(BspVertex) == 12, "Unexpected size of BspVertex");

	struct BspFace {
		uint16_t planeId = 0;
		int16_t side = 0;

		int32_t edgeId = 0;
		int16_t edgeCount = 0;
		int16_t textureId = 0;

		int32_t lightofsDay = 0;
		int32_t lightofsNight = 0;
	};
	static_assert(sizeof(BspFace) == 20, "Unexpected size of BspFace");

	struct BspEdge {
		int16_t vertexIndices[2]{0, 0}; // negative means counter clock wise
	};
	static_assert(sizeof(BspEdge) == 4, "Unexpected size of BspEdge");

	struct Quake1Texinfo {
		char name[16];
		uint32_t width, height; // must be a multiple of 8
		uint32_t offset1; // offset to u_char Pix[width   * height]
		uint32_t offset2; // offset to u_char Pix[width/2 * height/2]
		uint32_t offset4; // offset to u_char Pix[width/4 * height/4]
		uint32_t offset8; // offset to u_char Pix[width/8 * height/8]
	};

	// ----------------------------------------------
	// structures used for loading the relevant parts

	struct Model {
		int32_t faceId = 0;
		int32_t faceCount = 0;
	};

	struct Face {
		int32_t edgeId = 0;
		int16_t edgeCount = 0;
		int16_t textureId = 0; // texture info idx
		int32_t index = -1;
		bool used = false;
	};

	struct Texture : public BspTexture {
		image::ImagePtr image;
	};

	bool voxelize(const core::DynamicArray<Texture> &textures, const core::DynamicArray<Face> &faces,
				  const core::DynamicArray<BspEdge> &edges, const core::DynamicArray<int32_t> &surfEdges,
				  const core::DynamicArray<BspVertex> &vertices, scenegraph::SceneGraph &sceneGraph,
				  voxel::PaletteLookup &palLookup, const core::String &name);

	int32_t validateLump(const BspLump &lump, size_t elementSize) const;

	bool loadQuake1Bsp(const core::String &filename, io::SeekableReadStream &stream, scenegraph::SceneGraph &sceneGraph,
					   const BspHeader &header);
	bool loadQuake1Textures(const core::String &filename, io::SeekableReadStream &stream, const BspHeader &header,
							core::DynamicArray<Texture> &textures, core::StringMap<image::ImagePtr> &textureMap);
	bool loadQuake1Faces(io::SeekableReadStream &stream, const BspHeader &header, core::DynamicArray<Face> &faces,
						 const core::DynamicArray<Texture> &textures);
	bool loadQuake1Edges(io::SeekableReadStream &stream, const BspHeader &header, core::DynamicArray<BspEdge> &edges,
						 core::DynamicArray<int32_t> &surfEdges);
	bool loadQuake1Vertices(io::SeekableReadStream &stream, const BspHeader &header,
							core::DynamicArray<BspVertex> &vertices);

	bool loadUFOAlienInvasionBsp(const core::String &filename, io::SeekableReadStream &stream,
								 scenegraph::SceneGraph &sceneGraph, const BspHeader &header);
	bool loadUFOAlienInvasionTextures(const core::String &filename, io::SeekableReadStream &stream,
									  const BspHeader &header, core::DynamicArray<Texture> &textures,
									  core::StringMap<image::ImagePtr> &textureMap);
	bool loadUFOAlienInvasionFaces(io::SeekableReadStream &stream, const BspHeader &header,
								   core::DynamicArray<Face> &faces);
	bool loadUFOAlienInvasionEdges(io::SeekableReadStream &stream, const BspHeader &header,
								   core::DynamicArray<BspEdge> &edges, core::DynamicArray<int32_t> &surfEdges);
	bool loadUFOAlienInvasionVertices(io::SeekableReadStream &stream, const BspHeader &header,
									  core::DynamicArray<BspVertex> &vertices);
	bool loadUFOAlienInvasionFacesForLevel(io::SeekableReadStream &stream, const BspHeader &header,
										   core::DynamicArray<Face> &faces, core::DynamicArray<Face> &facesLevel,
										   const core::DynamicArray<Model> &models, int level);
	bool loadUFOAlienInvasionModels(io::SeekableReadStream &stream, const BspHeader &header,
									core::DynamicArray<Model> &models);

	bool voxelizeGroups(const core::String &filename, io::SeekableReadStream &stream,
						scenegraph::SceneGraph &sceneGraph, const LoadContext &ctx) override;

public:
	bool saveMeshes(const core::Map<int, int> &, const scenegraph::SceneGraph &, const Meshes &meshes,
					const core::String &filename, io::SeekableWriteStream &stream, const glm::vec3 &scale, bool quad,
					bool withColor, bool withTexCoords) override {
		return false;
	}
};

} // namespace voxelformat
