/**
 * @file
 */

#pragma once

#include "core/collection/Array.h"
#include "core/collection/DynamicArray.h"
#include "io/Stream.h"
#include "voxel/RawVolume.h"
#include "io/File.h"
#include "image/Image.h"
#include "SceneGraph.h"
#include "core/RGBA.h"
#include "voxel/Palette.h"
#include <glm/fwd.hpp>

namespace voxel {
	class Mesh;
}

namespace voxelformat {

// the max amount of voxels - [0-255]
static constexpr int MaxRegionSize = 256;

/**
 * @brief Base class for all voxel formats.
 */
class Format {
protected:
	/**
	 * @brief Checks whether the given chunk is empty (only contains air).
	 *
	 * @param v The volume
	 * @param maxSize The chunk size
	 * @param x The chunk position
	 * @param y The chunk position
	 * @param z The chunk position
	 */
	bool isEmptyBlock(const voxel::RawVolume *v, const glm::ivec3 &maxSize, int x, int y, int z) const;
	/**
	 * @brief Calculate the boundaries while aligning them to the given @c maxSize. This ensures that the
	 * calculated extends are exactly @c maxSize when iterating over them (and align relative to 0,0,0 and
	 * @c maxSize).
	 *
	 * @param[in] region The region to calculate the aligned mins/maxs for
	 * @param[in] maxSize The size of a single chunk to align with.
	 * @param[out] mins The extends of the aabb aligned with @c maxSize
	 * @param[out] maxs The extends of the aabb aligned with @c maxSize
	 */
	void calcMinsMaxs(const voxel::Region& region, const glm::ivec3 &maxSize, glm::ivec3 &mins, glm::ivec3 &maxs) const;
	/**
	 * @brief Split volumes according to their max size into several smaller volumes
	 * Some formats only support small volumes sizes per object - but multiple objects.
	 */
	void splitVolumes(const SceneGraph& srcSceneGraph, SceneGraph& destSceneGraph, const glm::ivec3 &maxSize, bool crop = false);
	// TODO: unused atm
	voxel::RawVolume* transformVolume(const SceneGraphTransform &t, const voxel::RawVolume *in) const;

public:
	virtual ~Format() = default;

	virtual image::ImagePtr loadScreenshot(const core::String &filename, io::SeekableReadStream& stream);

	/**
	 * @brief Only load the palette that is included in the format
	 * @note Not all voxel formats have a palette included
	 *
	 * @return the amount of colors found in the palette
	 */
	virtual size_t loadPalette(const core::String &filename, io::SeekableReadStream& stream, voxel::Palette &palette);

	/**
	 * @brief If the format supports multiple layers or groups, this method will give them to you as single volumes
	 */
	virtual bool loadGroups(const core::String &filename, io::SeekableReadStream& stream, SceneGraph& sceneGraph) = 0;
	virtual bool saveGroups(const SceneGraph& sceneGraph, const core::String &filename, io::SeekableWriteStream& stream) = 0;
	virtual bool save(const voxel::RawVolume* volume, const core::String &filename, io::SeekableWriteStream& stream);
};

class NoColorFormat : public Format {
};

class PaletteFormat : public Format {
protected:
	/**
	 * This is the loaded palette from the input file. This is not the currently in-use palette. This might differ
	 * and the colors will get matched to the in-use palette.
	 */
	voxel::Palette _palette;
public:
	size_t loadPalette(const core::String &filename, io::SeekableReadStream& stream, voxel::Palette &palette) override;
};

class RGBAFormat : public Format {
};

}
