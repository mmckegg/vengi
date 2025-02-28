/**
 * @file
 */

#include "SceneManager.h"

#include "app/Async.h"
#include "command/Command.h"
#include "command/CommandCompleter.h"
#include "core/ArrayLength.h"
#include "core/Color.h"
#include "core/GLM.h"
#include "app/I18N.h"
#include "core/Log.h"
#include "core/String.h"
#include "core/StringUtil.h"
#include "core/TimeProvider.h"
#include "core/collection/DynamicArray.h"
#include "io/Archive.h"
#include "io/File.h"
#include "io/FileStream.h"
#include "io/Filesystem.h"
#include "io/FilesystemArchive.h"
#include "io/FormatDescription.h"
#include "io/MemoryArchive.h"
#include "io/Stream.h"
#include "math/Axis.h"
#include "math/Random.h"
#include "math/Ray.h"
#include "metric/MetricFacade.h"
#include "scenegraph/SceneGraphAnimation.h"
#include "scenegraph/SceneGraphKeyFrame.h"
#include "video/Camera.h"
#include "voxel/Face.h"
#include "voxel/MaterialColor.h"
#include "palette/Palette.h"
#include "palette/PaletteLookup.h"
#include "voxel/RawVolume.h"
#include "voxel/RawVolumeWrapper.h"
#include "voxel/Voxel.h"
#include "voxelfont/VoxelFont.h"
#include "voxelformat/Format.h"
#include "scenegraph/SceneGraph.h"
#include "scenegraph/SceneGraphNode.h"
#include "scenegraph/SceneGraphUtil.h"
#include "voxelformat/VolumeFormat.h"
#include "voxelgenerator/TreeGenerator.h"
#include "voxelrender/ImageGenerator.h"
#include "voxelrender/RawVolumeRenderer.h"
#include "voxelrender/SceneGraphRenderer.h"
#include "voxelutil/Picking.h"
#include "voxelutil/Raycast.h"
#include "voxelutil/VolumeCropper.h"
#include "voxelutil/VolumeRescaler.h"
#include "voxelutil/VolumeResizer.h"
#include "voxelutil/VolumeRotator.h"
#include "voxelutil/VolumeSplitter.h"
#include "voxelutil/VolumeVisitor.h"
#include "voxelutil/VoxelUtil.h"
#include "voxelutil/ImageUtils.h"

#include "Config.h"
#include "MementoHandler.h"
#include "SceneUtil.h"
#include "Clipboard.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace voxedit {

inline auto nodeCompleter(const scenegraph::SceneGraph &sceneGraph) {
	return [&] (const core::String& str, core::DynamicArray<core::String>& matches) -> int {
		int i = 0;
		for (auto iter = sceneGraph.beginAllModels(); iter != sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &modelNode = *iter;
			matches.push_back(core::string::toString(modelNode.id()));
		}
		return i;
	};
}

inline auto paletteCompleter() {
	return [&] (const core::String& str, core::DynamicArray<core::String>& matches) -> int {
		int i = 0;
		for (i = 0; i < lengthof(palette::Palette::builtIn); ++i) {
			if (core::string::startsWith(palette::Palette::builtIn[i], str.c_str())) {
				matches.push_back(palette::Palette::builtIn[i]);
			}
		}
		return i;
	};
}

SceneManager::SceneManager(const core::TimeProviderPtr &timeProvider, const io::FilesystemPtr &filesystem,
						   const SceneRendererPtr &sceneRenderer, const ModifierRendererPtr &modifierRenderer)
	: _timeProvider(timeProvider), _sceneRenderer(sceneRenderer), _modifierFacade(this, modifierRenderer),
	  _luaApi(filesystem), _filesystem(filesystem) {
}

SceneManager::~SceneManager() {
	core_assert_msg(_initialized == 0, "SceneManager was not properly shut down");
}

bool SceneManager::loadPalette(const core::String& paletteName, bool searchBestColors, bool save) {
	palette::Palette palette;

	const bool isNodePalette = core::string::startsWith(paletteName, "node:");
	if (isNodePalette) {
		const size_t nodeDetails = paletteName.rfind("##");
		if (nodeDetails != core::String::npos) {
			const int nodeId = core::string::toInt(paletteName.substr(nodeDetails + 2, paletteName.size()));
			if (_sceneGraph.hasNode(nodeId)) {
				palette = _sceneGraph.node(nodeId).palette();
			} else {
				Log::warn("Couldn't find palette for node %i", nodeId);
			}
		}
	}

	if (palette.colorCount() == 0 && !palette.load(paletteName.c_str())) {
		return false;
	}
	if (!setActivePalette(palette, searchBestColors)) {
		return false;
	}
	core::Var::getSafe(cfg::VoxEditLastPalette)->setVal(paletteName);

	if (save && !isNodePalette && !palette.isBuiltIn()) {
		const core::String filename = core::string::extractFilename(palette.name());
		const core::String &paletteFilename = core::string::format("palette-%s.png", filename.c_str());
		const io::FilePtr &pngFile = _filesystem->open(paletteFilename, io::FileMode::Write);
		if (!palette.save(pngFile->name().c_str())) {
			Log::warn("Failed to write palette image: %s", paletteFilename.c_str());
		}
	}

	return true;
}

bool SceneManager::importPalette(const core::String& file) {
	palette::Palette palette;
	if (!voxelformat::importPalette(file, palette)) {
		Log::warn("Failed to import a palette from file '%s'", file.c_str());
		return false;
	}

	core::String paletteName(core::string::extractFilename(file.c_str()));
	const core::String &paletteFilename = core::string::format("palette-%s.png", paletteName.c_str());
	const io::FilePtr &pngFile = _filesystem->open(paletteFilename, io::FileMode::Write);
	if (palette.save(pngFile->name().c_str())) {
		core::Var::getSafe(cfg::VoxEditLastPalette)->setVal(paletteName);
	} else {
		Log::warn("Failed to write palette image");
	}

	return setActivePalette(palette);
}

bool SceneManager::importAsVolume(const core::String &file, int maxDepth, bool bothSides) {
	const image::ImagePtr& img = image::loadImage(file);
	const palette::Palette &palette = activePalette();
	voxel::RawVolume *v = voxelutil::importAsVolume(img, palette, maxDepth, bothSides);
	if (v == nullptr) {
		return false;
	}
	scenegraph::SceneGraphNode newNode;
	newNode.setVolume(v, true);
	newNode.setName(core::string::extractFilename(img->name().c_str()));
	newNode.setPalette(palette);
	return moveNodeToSceneGraph(newNode) != InvalidNodeId;
}

bool SceneManager::importAsPlane(const core::String& file) {
	const image::ImagePtr& img = image::loadImage(file);
	const palette::Palette &palette = activePalette();
	voxel::RawVolume *v = voxelutil::importAsPlane(img, palette);
	if (v == nullptr) {
		return false;
	}
	scenegraph::SceneGraphNode newNode;
	newNode.setVolume(v, true);
	newNode.setName(core::string::extractFilename(img->name().c_str()));
	newNode.setPalette(palette);
	return moveNodeToSceneGraph(newNode) != InvalidNodeId;
}

bool SceneManager::importHeightmap(const core::String& file) {
	const int nodeId = activeNode();
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return false;
	}
	const image::ImagePtr& img = image::loadImage(file);
	if (!img->isLoaded()) {
		return false;
	}
	voxel::RawVolumeWrapper wrapper(v);
	const voxel::Voxel dirtVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 1);
	const voxel::Voxel grassVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 2);
	voxelutil::importHeightmap(wrapper, img, dirtVoxel, grassVoxel);
	modified(nodeId, wrapper.dirtyRegion());
	return true;
}

bool SceneManager::importColoredHeightmap(const core::String& file) {
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (!node->isModelNode()) {
		Log::error("Selected node is no model node - failed to import heightmap");
		return false;
	}
	core_assert(node != nullptr);
	voxel::RawVolume* v = node->volume();
	if (v == nullptr) {
		return false;
	}
	const image::ImagePtr& img = image::loadImage(file);
	if (!img->isLoaded()) {
		return false;
	}
	voxel::RawVolumeWrapper wrapper(v);
	palette::PaletteLookup palLookup(node->palette());
	const voxel::Voxel dirtVoxel = voxel::createVoxel(voxel::VoxelType::Generic, 0);
	voxelutil::importColoredHeightmap(wrapper, palLookup, img, dirtVoxel);
	modified(nodeId, wrapper.dirtyRegion());
	return true;
}

void SceneManager::autosave() {
	if (!_needAutoSave) {
		return;
	}
	const int delay = _autoSaveSecondsDelay->intVal();
	if (delay <= 0 || _lastAutoSave + (double)delay > _timeProvider->tickSeconds()) {
		return;
	}
	io::FileDescription autoSaveFilename;
	if (_lastFilename.empty()) {
		autoSaveFilename.set("autosave-noname." + voxelformat::vengi().mainExtension());
	} else {
		if (core::string::startsWith(_lastFilename.c_str(), "autosave-")) {
			autoSaveFilename = _lastFilename;
		} else {
			const io::FilePtr file = _filesystem->open(_lastFilename.name);
			const core::String& p = file->path();
			const core::String& f = file->fileName();
			const core::String& e = file->extension();
			autoSaveFilename.set(core::string::format("%sautosave-%s.%s",
					p.c_str(), f.c_str(), e.c_str()), &_lastFilename.desc);
		}
	}
	if (save(autoSaveFilename, true)) {
		Log::info("Autosave file %s", autoSaveFilename.c_str());
	} else {
		Log::warn("Failed to autosave");
	}
	_lastAutoSave = _timeProvider->tickSeconds();
}

bool SceneManager::saveNode(int nodeId, const core::String& file) {
	const scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		Log::warn("Node with id %i wasn't found", nodeId);
		return true;
	}
	if (node->type() != scenegraph::SceneGraphNodeType::Model) {
		Log::warn("Given node is no model node");
		return false;
	}
	scenegraph::SceneGraph newSceneGraph;
	scenegraph::SceneGraphNode newNode;
	scenegraph::copyNode(*node, newNode, false);
	if (node->isReference()) {
		newNode.setVolume(_sceneGraph.resolveVolume(*node));
	}
	newSceneGraph.emplace(core::move(newNode));
	voxelformat::SaveContext saveCtx;
	saveCtx.thumbnailCreator = voxelrender::volumeThumbnail;
	const io::ArchivePtr &archive = io::openFilesystemArchive(io::filesystem());
	if (voxelformat::saveFormat(newSceneGraph, file, &_lastFilename.desc, archive, saveCtx)) {
		Log::info("Saved node %i to %s", nodeId, file.c_str());
		return true;
	}
	Log::warn("Failed to save node %i to %s", nodeId, file.c_str());
	return false;
}

void SceneManager::fillHollow() {
	_sceneGraph.foreachGroup([&] (int nodeId) {
		scenegraph::SceneGraphNode *node = sceneGraphModelNode(nodeId);
		if (node == nullptr) {
			return;
		}
		voxel::RawVolume *v = node->volume();
		if (v == nullptr) {
			return;
		}
		voxel::RawVolumeWrapper wrapper = _modifierFacade.createRawVolumeWrapper(v);
		voxelutil::fillHollow(wrapper, _modifierFacade.cursorVoxel());
		modified(nodeId, wrapper.dirtyRegion());
	});
}

void SceneManager::fill() {
	_sceneGraph.foreachGroup([&](int nodeId) {
		scenegraph::SceneGraphNode *node = sceneGraphModelNode(nodeId);
		if (node == nullptr) {
			return;
		}
		voxel::RawVolume *v = node->volume();
		if (v == nullptr) {
			return;
		}
		voxel::RawVolumeWrapper wrapper = _modifierFacade.createRawVolumeWrapper(v);
		voxelutil::fill(wrapper, _modifierFacade.cursorVoxel(), _modifierFacade.isMode(ModifierType::Override));
		modified(nodeId, wrapper.dirtyRegion());
	});
}

void SceneManager::clear() {
	_sceneGraph.foreachGroup([&](int nodeId) {
		scenegraph::SceneGraphNode *node = sceneGraphModelNode(nodeId);
		if (node == nullptr) {
			return;
		}
		voxel::RawVolume *v = node->volume();
		if (v == nullptr) {
			return;
		}
		voxel::RawVolumeWrapper wrapper = _modifierFacade.createRawVolumeWrapper(v);
		voxelutil::clear(wrapper);
		modified(nodeId, wrapper.dirtyRegion());
	});
}

void SceneManager::hollow() {
	_sceneGraph.foreachGroup([&](int nodeId) {
		scenegraph::SceneGraphNode *node = sceneGraphModelNode(nodeId);
		if (node == nullptr) {
			return;
		}
		voxel::RawVolume *v = node->volume();
		if (v == nullptr) {
			return;
		}
		voxel::RawVolumeWrapper wrapper = _modifierFacade.createRawVolumeWrapper(v);
		voxelutil::hollow(wrapper);
		modified(nodeId, wrapper.dirtyRegion());
	});
}

void SceneManager::fillPlane(const image::ImagePtr &image) {
	const int nodeId = activeNode();
	if (nodeId == InvalidNodeId) {
		return;
	}
	voxel::RawVolume *v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::RawVolumeWrapper wrapper = _modifierFacade.createRawVolumeWrapper(v);
	const glm::ivec3 &pos = _modifierFacade.cursorPosition();
	const voxel::FaceNames face = _modifierFacade.cursorFace();
	const voxel::Voxel hitVoxel/* = hitCursorVoxel()*/; // TODO: should be an option
	voxelutil::fillPlane(wrapper, image, hitVoxel, pos, face);
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::nodeUpdateVoxelType(int nodeId, uint8_t palIdx, voxel::VoxelType newType) {
	voxel::RawVolume *v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::RawVolumeWrapper wrapper(v);
	voxelutil::visitVolume(wrapper, [&wrapper, palIdx, newType](int x, int y, int z, const voxel::Voxel &v) {
		if (v.getColor() != palIdx) {
			return;
		}
		wrapper.setVoxel(x, y, z, voxel::createVoxel(newType, palIdx));
	});
	modified(nodeId, wrapper.dirtyRegion());
}

bool SceneManager::saveModels(const core::String& dir) {
	bool state = false;
	for (auto iter = _sceneGraph.beginAllModels(); iter != _sceneGraph.end(); ++iter) {
		const scenegraph::SceneGraphNode &node = *iter;
		const core::String filename = core::string::path(dir, node.name() + ".vengi");
		state |= saveNode(node.id(), filename);
	}
	return state;
}

bool SceneManager::save(const io::FileDescription& file, bool autosave) {
	if (_sceneGraph.empty()) {
		Log::warn("No volumes for saving found");
		return false;
	}

	if (file.empty()) {
		Log::warn("No filename given for saving");
		return false;
	}
	voxelformat::SaveContext saveCtx;
	saveCtx.thumbnailCreator = voxelrender::volumeThumbnail;
	const io::ArchivePtr &archive = io::openFilesystemArchive(io::filesystem());
	if (voxelformat::saveFormat(_sceneGraph, file.name, &file.desc, archive, saveCtx)) {
		if (!autosave) {
			_dirty = false;
			_lastFilename = file;
			const core::String &ext = core::string::extractExtension(file.name);
			metric::count("save", 1, {{"type", ext}});
			core::Var::get(cfg::VoxEditLastFile)->setVal(file.name);
		}
		_needAutoSave = false;
		return true;
	}
	Log::warn("Failed to save to desired format");
	return false;
}

static void mergeIfNeeded(scenegraph::SceneGraph &newSceneGraph) {
	if (newSceneGraph.size() > voxel::MAX_VOLUMES) {
		const scenegraph::SceneGraph::MergedVolumePalette &merged = newSceneGraph.merge();
		newSceneGraph.clear();
		scenegraph::SceneGraphNode newNode;
		newNode.setVolume(merged.first, true);
		newNode.setPalette(merged.second);
		newSceneGraph.emplace(core::move(newNode));
	}
}

bool SceneManager::import(const core::String& file) {
	if (file.empty()) {
		Log::error("Can't import model: No file given");
		return false;
	}
	const io::ArchivePtr &archive = io::openFilesystemArchive(_filesystem);
	scenegraph::SceneGraph newSceneGraph;
	voxelformat::LoadContext loadCtx;
	io::FileDescription fileDesc;
	fileDesc.set(file);
	if (!voxelformat::loadFormat(fileDesc, archive, newSceneGraph, loadCtx)) {
		Log::error("Failed to load %s", file.c_str());
		return false;
	}
	mergeIfNeeded(newSceneGraph);

	scenegraph::SceneGraphNode groupNode(scenegraph::SceneGraphNodeType::Group);
	groupNode.setName(core::string::extractFilename(file));
	int newNodeId = _sceneGraph.emplace(core::move(groupNode), activeNode());
	bool state = false;
	for (auto iter = newSceneGraph.beginAllModels(); iter != newSceneGraph.end(); ++iter) {
		scenegraph::SceneGraphNode &node = *iter;
		state |= moveNodeToSceneGraph(node, newNodeId) != InvalidNodeId;
	}

	return state;
}

bool SceneManager::importDirectory(const core::String& directory, const io::FormatDescription *format, int depth) {
	if (directory.empty()) {
		return false;
	}
	const io::ArchivePtr &archive = io::openFilesystemArchive(_filesystem, directory);
	const core::DynamicArray<io::FilesystemEntry> &entities = archive->files();
	if (entities.empty()) {
		Log::info("Could not find any model in %s", directory.c_str());
		return false;
	}
	bool state = false;
	scenegraph::SceneGraphNode groupNode(scenegraph::SceneGraphNodeType::Group);
	groupNode.setName(core::string::extractFilename(directory));
	int importGroupNodeId = _sceneGraph.emplace(core::move(groupNode), activeNode());

	for (const auto &e : entities) {
		if (format == nullptr && !voxelformat::isModelFormat(e.name)) {
			continue;
		}
		scenegraph::SceneGraph newSceneGraph;
		io::FilePtr filePtr = _filesystem->open(e.fullPath, io::FileMode::SysRead);
		io::FileStream stream(filePtr);
		voxelformat::LoadContext loadCtx;
		io::FileDescription fileDesc;
		fileDesc.set(filePtr->name(), format);
		if (!voxelformat::loadFormat(fileDesc, archive, newSceneGraph, loadCtx)) {
			Log::error("Failed to load %s", e.fullPath.c_str());
		} else {
			mergeIfNeeded(newSceneGraph);
			for (auto iter = newSceneGraph.beginModel(); iter != newSceneGraph.end(); ++iter) {
				scenegraph::SceneGraphNode &node = *iter;
				state |= moveNodeToSceneGraph(node, importGroupNodeId) != InvalidNodeId;
			}
		}
	}
	return state;
}

bool SceneManager::load(const io::FileDescription& file) {
	if (file.empty()) {
		return false;
	}
	if (_loadingFuture.valid()) {
		Log::error("Failed to load '%s' - still loading another model", file.c_str());
		return false;
	}
	const io::ArchivePtr &archive = io::openFilesystemArchive(_filesystem);
	_loadingFuture = app::async([archive, file] () {
		scenegraph::SceneGraph newSceneGraph;
		voxelformat::LoadContext loadCtx;
		voxelformat::loadFormat(file, archive, newSceneGraph, loadCtx);
		mergeIfNeeded(newSceneGraph);
		/**
		 * @todo stuff that happens in MeshState::scheduleRegionExtraction() and
		 * MeshState::runScheduledExtractions() should happen here
		 */
		return core::move(newSceneGraph);
	});
	_lastFilename.set(file.name, &file.desc);
	return true;
}

bool SceneManager::load(const io::FileDescription& file, const uint8_t *data, size_t size) {
	scenegraph::SceneGraph newSceneGraph;
	io::MemoryArchivePtr archive = io::openMemoryArchive();
	archive->add(file.name, data, size);
	voxelformat::LoadContext loadCtx;
	voxelformat::loadFormat(file, archive, newSceneGraph, loadCtx);
	mergeIfNeeded(newSceneGraph);
	if (loadSceneGraph(core::move(newSceneGraph))) {
		_needAutoSave = false;
		_dirty = false;
		_lastFilename.clear();
	}
	return true;
}

void SceneManager::setMousePos(int x, int y) {
	if (_mouseCursor.x == x && _mouseCursor.y == y) {
		return;
	}
	_mouseCursor.x = x;
	_mouseCursor.y = y;
	// moving the mouse would trigger mouse tracing again
	// TODO: maybe only do this if a mouse button was pressed?
	_traceViaMouse = true;
}

bool SceneManager::supportsEditMode() const {
	const int nodeId = _sceneGraph.activeNode();
	const scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	return node.isModelNode();
}

void SceneManager::modified(int nodeId, const voxel::Region& modifiedRegion, bool markUndo, uint64_t renderRegionMillis) {
	Log::debug("Modified node %i, record undo state: %s", nodeId, markUndo ? "true" : "false");
	voxel::logRegion("Modified", modifiedRegion);
	if (markUndo) {
		scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
		_mementoHandler.markModification(node, modifiedRegion);
	}
	if (modifiedRegion.isValid()) {
		_sceneRenderer->updateNodeRegion(nodeId, modifiedRegion, renderRegionMillis);
	}
	markDirty();
	resetLastTrace();
}

void SceneManager::colorToNewNode(const voxel::Voxel voxelColor) {
	const int nodeId = _sceneGraph.activeNode();
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	voxel::RawVolume *v = _sceneGraph.resolveVolume(node);
	if (v == nullptr) {
		return;
	}
	const voxel::Region &region = v->region();
	voxel::RawVolume* newVolume = new voxel::RawVolume(region);
	voxel::RawVolumeWrapper wrapper(v);
	voxelutil::visitVolume(wrapper, [&] (int32_t x, int32_t y, int32_t z, const voxel::Voxel& voxel) {
		if (voxel.getColor() == voxelColor.getColor()) {
			newVolume->setVoxel(x, y, z, voxel);
			wrapper.setVoxel(x, y, z, voxel::Voxel());
		}
	});
	modified(nodeId, wrapper.dirtyRegion());
	scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
	copyNode(node, newNode, false, true);
	newNode.setVolume(newVolume, true);
	newNode.setName(core::string::format("color: %i", (int)voxelColor.getColor()));
	moveNodeToSceneGraph(newNode, node.parent());
}

void SceneManager::scaleUp(int nodeId) {
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::RawVolume *destVolume = voxelutil::scaleUp(*v);
	if (destVolume == nullptr) {
		return;
	}
	if (!setNewVolume(nodeId, destVolume, true)) {
		delete destVolume;
		return;
	}
	modified(nodeId, destVolume->region());
}

void SceneManager::scaleDown(int nodeId) {
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	const voxel::Region srcRegion = v->region();
	const glm::ivec3& targetDimensionsHalf = (srcRegion.getDimensionsInVoxels() / 2) - 1;
	if (targetDimensionsHalf.x < 0 || targetDimensionsHalf.y < 0 || targetDimensionsHalf.z < 0) {
		Log::debug("Can't scale anymore");
		return;
	}
	const voxel::Region destRegion(srcRegion.getLowerCorner(), srcRegion.getLowerCorner() + targetDimensionsHalf);
	voxel::RawVolume* destVolume = new voxel::RawVolume(destRegion);
	voxelutil::scaleDown(*v, _sceneGraph.node(nodeId).palette(), *destVolume);
	if (!setNewVolume(nodeId, destVolume, true)) {
		delete destVolume;
		return;
	}
	modified(nodeId, srcRegion);
}

void SceneManager::splitObjects() {
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode* node = sceneGraphModelNode(nodeId);
	if (node == nullptr) {
		return;
	}
	core::DynamicArray<voxel::RawVolume *> volumes;
	voxelutil::splitObjects(node->volume(), volumes);
	if (volumes.empty()) {
		return;
	}

	for (voxel::RawVolume *newVolume : volumes) {
		scenegraph::SceneGraphNode newNode;
		newNode.setVolume(newVolume, true);
		newNode.setName(node->name());
		newNode.setPalette(node->palette());
		moveNodeToSceneGraph(newNode, nodeId);
	}
}

void SceneManager::crop() {
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode* node = sceneGraphModelNode(nodeId);
	if (node == nullptr) {
		return;
	}
	voxel::RawVolume* newVolume = voxelutil::cropVolume(node->volume());
	if (newVolume == nullptr) {
		return;
	}
	if (!setNewVolume(nodeId, newVolume, true)) {
		delete newVolume;
		return;
	}
	modified(nodeId, newVolume->region());
}

void SceneManager::nodeResize(int nodeId, const glm::ivec3& size) {
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::Region region = v->region();
	region.shiftUpperCorner(size);
	nodeResize(nodeId, region);
}

void SceneManager::nodeResize(int nodeId, const voxel::Region &region) {
	if (!region.isValid()) {
		return;
	}
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		Log::error("Failed to lookup volume for node %i", nodeId);
		return;
	}
	const voxel::Region oldRegion = v->region();
	Log::debug("Resize volume from %s to %s", oldRegion.toString().c_str(), region.toString().c_str());
	voxel::RawVolume* newVolume = voxelutil::resize(v, region);
	if (newVolume == nullptr) {
		return;
	}
	if (!setNewVolume(nodeId, newVolume, false)) {
		delete newVolume;
		return;
	}
	const glm::ivec3 oldMins = oldRegion.getLowerCorner();
	const glm::ivec3 oldMaxs = oldRegion.getUpperCorner();
	const glm::ivec3 mins = region.getLowerCorner();
	const glm::ivec3 maxs = region.getUpperCorner();
	if (glm::all(glm::greaterThanEqual(maxs, oldMaxs)) && glm::all(glm::lessThanEqual(mins, oldMins))) {
		// we don't have to re-extract a mesh if only new empty voxels were added.
		modified(nodeId, voxel::Region::InvalidRegion);
	} else {
		// TODO: assemble the 6 surroundings to optimize this for big volumes
		modified(nodeId, newVolume->region());
	}

	if (activeNode() == nodeId) {
		const glm::ivec3 &refPos = referencePosition();
		if (!region.containsPoint(refPos)) {
			setReferencePosition(region.getCenter());
		}
	}
}

void SceneManager::resizeAll(const glm::ivec3& size) {
	_sceneGraph.foreachGroup([&] (int nodeId) {
		nodeResize(nodeId, size);
	});
}

voxel::RawVolume* SceneManager::volume(int nodeId) {
	if (nodeId == InvalidNodeId) {
		return nullptr;
	}
	if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
		return _sceneGraph.resolveVolume(*node);
	}
	return nullptr;
}

const voxel::RawVolume* SceneManager::volume(int nodeId) const {
	const scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId);
	core_assert_msg(node != nullptr, "Node with id %i wasn't found in the scene graph", nodeId);
	if (node == nullptr) {
		return nullptr;
	}
	return _sceneGraph.resolveVolume(*node);
}

int SceneManager::activeNode() const {
	return _sceneGraph.activeNode();
}

palette::Palette &SceneManager::activePalette() const {
	const int nodeId = activeNode();
	if (!_sceneGraph.hasNode(nodeId)) {
		return _sceneGraph.firstPalette();
	}
	return _sceneGraph.node(nodeId).palette();
}

bool SceneManager::setActivePalette(const palette::Palette &palette, bool searchBestColors) {
	const int nodeId = activeNode();
	if (!_sceneGraph.hasNode(nodeId)) {
		Log::warn("Failed to set the active palette - node with id %i not found", nodeId);
		return false;
	}
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.type() != scenegraph::SceneGraphNodeType::Model) {
		Log::warn("Failed to set the active palette - node with id %i is no model node", nodeId);
		return false;
	}
	if (searchBestColors) {
		const voxel::Region dirtyRegion = node.remapToPalette(palette);
		if (!dirtyRegion.isValid()) {
			Log::warn("Remapping palette indices failed");
			return false;
		}
		modified(nodeId, dirtyRegion);
	}
	_mementoHandler.markPaletteChange(node);
	node.setPalette(palette);
	return true;
}

voxel::RawVolume* SceneManager::activeVolume() {
	const int nodeId = activeNode();
	if (nodeId == InvalidNodeId) {
		Log::error("No active node in scene graph");
		return nullptr;
	}
	return volume(nodeId);
}

bool SceneManager::mementoRename(const MementoState& s) {
	Log::debug("Memento: rename of node %i (%s)", s.nodeId, s.name.c_str());
	return nodeRename(s.nodeId, s.name);
}

bool SceneManager::mementoProperties(const MementoState& s) {
	Log::debug("Memento: properties of node %i (%s)", s.nodeId, s.name.c_str());
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(s.nodeId)) {
		node->properties().clear();
		core_assert(s.properties.hasValue());
		node->addProperties(*s.properties.value());
		return true;
	}
	return false;
}

bool SceneManager::mementoKeyFrames(const MementoState& s) {
	Log::debug("Memento: keyframes of node %i (%s)", s.nodeId, s.name.c_str());
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(s.nodeId)) {
		node->setAllKeyFrames(*s.keyFrames.value(), _sceneGraph.activeAnimation());
		if (s.pivot.hasValue()) {
			node->setPivot(*s.pivot.value());
		}
		_sceneGraph.updateTransforms();
		return true;
	}
	return false;
}

bool SceneManager::mementoPaletteChange(const MementoState& s) {
	Log::debug("Memento: palette change of node %i to %s", s.nodeId, s.name.c_str());
	if (scenegraph::SceneGraphNode* node = sceneGraphNode(s.nodeId)) {
		node->setPalette(*s.palette.value());
		if (s.hasVolumeData()) {
			mementoModification(s);
		}
		markDirty();
		return true;
	}
	return false;
}

bool SceneManager::mementoTransform(const MementoState& s) {
	Log::debug("Memento: transform of node %i", s.nodeId);
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(s.nodeId)) {
		if (s.pivot.hasValue()) {
			node->setPivot(*s.pivot.value());
		}
		scenegraph::SceneGraphTransform &transform = node->keyFrame(s.keyFrameIdx).transform();
		if (s.worldMatrix.hasValue()) {
			transform.setWorldMatrix(*s.worldMatrix.value());
			transform.update(_sceneGraph, *node, s.keyFrameIdx, true);
			return true;
		}
	}
	return false;
}

bool SceneManager::mementoModification(const MementoState& s) {
	Log::debug("Memento: modification in volume of node %i (%s)", s.nodeId, s.name.c_str());
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(s.nodeId)) {
		if (node->type() == scenegraph::SceneGraphNodeType::Model && s.nodeType == scenegraph::SceneGraphNodeType::ModelReference) {
			node->setReference(s.referenceId, true);
		} else {
			if (node->type() == scenegraph::SceneGraphNodeType::ModelReference && s.nodeType == scenegraph::SceneGraphNodeType::Model) {
				node->unreferenceModelNode(_sceneGraph.node(node->reference()));
			}
			if (node->region() != s.dataRegion()) {
				voxel::RawVolume *v = new voxel::RawVolume(s.dataRegion());
				if (!setSceneGraphNodeVolume(*node, v)) {
					delete v;
				}
			}
			MementoData::toVolume(node->volume(), s.data);
		}
		node->setName(s.name);
		if (s.pivot.hasValue()) {
			node->setPivot(*s.pivot.value());
		}
		if (s.palette.hasValue()) {
			node->setPalette(*s.palette.value());
		}
		modified(node->id(), s.data.region(), false);
		return true;
	}
	Log::warn("Failed to handle memento state - node id %i not found (%s)", s.nodeId, s.name.c_str());
	return false;
}

bool SceneManager::mementoStateToNode(const MementoState &s) {
	scenegraph::SceneGraphNodeType type = s.nodeType;
	if (type == scenegraph::SceneGraphNodeType::Max) {
		if (!s.hasVolumeData()) {
			type = scenegraph::SceneGraphNodeType::Group;
		} else {
			type = scenegraph::SceneGraphNodeType::Model;
		}
	}
	scenegraph::SceneGraphNode newNode(type);
	if (type == scenegraph::SceneGraphNodeType::Model) {
		newNode.setVolume(new voxel::RawVolume(s.dataRegion()), true);
		MementoData::toVolume(newNode.volume(), s.data);
	}
	if (s.palette.hasValue()) {
		newNode.setPalette(*s.palette.value());
	}
	if (type == scenegraph::SceneGraphNodeType::ModelReference) {
		newNode.setReference(s.referenceId);
	}
	if (s.keyFrames.hasValue()) {
		newNode.setAllKeyFrames(*s.keyFrames.value(), _sceneGraph.activeAnimation());
	}
	if (s.properties.hasValue()) {
		newNode.properties().clear();
		newNode.addProperties(*s.properties.value());
	}
	if (s.pivot.hasValue()) {
		newNode.setPivot(*s.pivot.value());
	}
	newNode.setName(s.name);
	const int newNodeId = moveNodeToSceneGraph(newNode, s.parentId);
	_mementoHandler.updateNodeId(s.nodeId, newNodeId);
	_sceneGraph.updateTransforms();
	return newNodeId != InvalidNodeId;
}

bool SceneManager::mementoStateExecute(const MementoState &s, bool isRedo) {
	core_assert(s.valid());
	ScopedMementoHandlerLock lock(_mementoHandler);
	if (s.type == MementoType::SceneNodeRenamed) {
		return mementoRename(s);
	}
	if (s.type == MementoType::SceneNodeKeyFrames) {
		return mementoKeyFrames(s);
	}
	if (s.type == MementoType::SceneNodeProperties) {
		return mementoProperties(s);
	}
	if (s.type == MementoType::SceneNodePaletteChanged) {
		return mementoPaletteChange(s);
	}
	if (s.type == MementoType::SceneNodeMove) {
		Log::debug("Memento: move of node %i (%s) (new parent %i)", s.nodeId, s.name.c_str(), s.parentId);
		return nodeMove(s.nodeId, s.parentId);
	}
	if (s.type == MementoType::SceneNodeTransform) {
		return mementoTransform(s);
	}
	if (s.type == MementoType::Modification) {
		return mementoModification(s);
	}
	if (isRedo) {
		if (s.type == MementoType::SceneNodeRemoved) {
			Log::debug("Memento: remove of node %i (%s) from parent %i", s.nodeId, s.name.c_str(), s.parentId);
			return nodeRemove(s.nodeId, true);
		}
		if (s.type == MementoType::SceneNodeAdded) {
			Log::debug("Memento: add node (%s) to parent %i", s.name.c_str(), s.parentId);
			return mementoStateToNode(s);
		}
	} else {
		if (s.type == MementoType::SceneNodeRemoved) {
			Log::debug("Memento: remove of node (%s) from parent %i", s.name.c_str(), s.parentId);
			return mementoStateToNode(s);
		}
		if (s.type == MementoType::SceneNodeAdded) {
			Log::debug("Memento: add node (%s) to parent %i", s.name.c_str(), s.parentId);
			return nodeRemove(s.nodeId, true);
		}
	}
	return true;
}

bool SceneManager::undo(int n) {
	Log::debug("undo %i steps", n);
	for (int i = 0; i < n; ++i) {
		if (!doUndo()) {
			return false;
		}
	}
	return true;
}

bool SceneManager::redo(int n) {
	Log::debug("redo %i steps", n);
	for (int i = 0; i < n; ++i) {
		if (!doRedo()) {
			return false;
		}
	}
	return true;
}

bool SceneManager::doUndo() {
	if (!mementoHandler().canUndo()) {
		Log::debug("Nothing to undo");
		return false;
	}

	const MementoState& s = _mementoHandler.undo();
	return mementoStateExecute(s, false);
}

bool SceneManager::doRedo() {
	if (!mementoHandler().canRedo()) {
		Log::debug("Nothing to redo");
		return false;
	}

	const MementoState& s = _mementoHandler.redo();
	return mementoStateExecute(s, true);
}

bool SceneManager::saveSelection(const io::FileDescription& file) {
	const Selections& selections = _modifierFacade.selections();
	if (selections.empty()) {
		return false;
	}
	const int nodeId = activeNode();
	const scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		Log::warn("Node with id %i wasn't found", nodeId);
		return true;
	}
	if (node->type() != scenegraph::SceneGraphNodeType::Model) {
		Log::warn("Given node is no model node");
		return false;
	}
	const io::ArchivePtr &archive = io::openFilesystemArchive(io::filesystem());
	for (const Selection &selection : selections) {
		scenegraph::SceneGraph newSceneGraph;
		scenegraph::SceneGraphNode newNode;
		scenegraph::copyNode(*node, newNode, false);
		newNode.setVolume(new voxel::RawVolume(_sceneGraph.resolveVolume(*node), selection), true);
		newSceneGraph.emplace(core::move(newNode));
		voxelformat::SaveContext saveCtx;
		saveCtx.thumbnailCreator = voxelrender::volumeThumbnail;
		if (voxelformat::saveFormat(newSceneGraph, file.name, &file.desc, archive, saveCtx)) {
			Log::info("Saved node %i to %s", nodeId, file.name.c_str());
		} else {
			Log::warn("Failed to save node %i to %s", nodeId, file.name.c_str());
			return false;
		}
	}
	return true;
}

bool SceneManager::copy() {
	const Selections& selections = _modifierFacade.selections();
	if (selections.empty()) {
		return false;
	}
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.volume() == nullptr) {
		return false;
	}
	voxel::VoxelData voxelData(node.volume(), node.palette(), false);
	_copy = voxedit::tool::copy(voxelData, selections);
	return _copy;
}

bool SceneManager::pasteAsNewNode() {
	if (!_copy) {
		Log::debug("Nothing copied yet - failed to paste");
		return false;
	}
	const int nodeId = activeNode();
	const scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
	scenegraph::copyNode(node, newNode, false);
	newNode.setVolume(new voxel::RawVolume(*_copy.volume), true);
	newNode.setPalette(*_copy.palette);
	return moveNodeToSceneGraph(newNode, node.parent()) != InvalidNodeId;
}

bool SceneManager::paste(const glm::ivec3& pos) {
	if (!_copy) {
		Log::debug("Nothing copied yet - failed to paste");
		return false;
	}
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.volume() == nullptr) {
		return false;
	}
	voxel::Region modifiedRegion;
	voxel::VoxelData voxelData(node.volume(), node.palette(), false);
	voxedit::tool::paste(voxelData, _copy, pos, modifiedRegion);
	if (!modifiedRegion.isValid()) {
		Log::debug("Failed to paste");
		return false;
	}
	const int64_t dismissMillis = core::Var::getSafe(cfg::VoxEditModificationDismissMillis)->intVal();
	modified(nodeId, modifiedRegion, true, dismissMillis);
	return true;
}

bool SceneManager::cut() {
	const Selections& selections = _modifierFacade.selections();
	if (selections.empty()) {
		Log::debug("Nothing selected - failed to cut");
		return false;
	}
	const int nodeId = activeNode();
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.volume() == nullptr) {
		return false;
	}
	voxel::Region modifiedRegion;
	voxel::VoxelData voxelData(node.volume(), node.palette(), false);
	_copy = voxedit::tool::cut(voxelData, selections, modifiedRegion);
	if (!_copy) {
		Log::debug("Failed to cut");
		return false;
	}
	if (!modifiedRegion.isValid()) {
		Log::debug("Failed to cut");
		_copy = {};
		return false;
	}
	const int64_t dismissMillis = core::Var::getSafe(cfg::VoxEditModificationDismissMillis)->intVal();
	modified(nodeId, modifiedRegion, true, dismissMillis);
	return true;
}

void SceneManager::resetLastTrace() {
	if (!_traceViaMouse) {
		return;
	}
	_lastRaytraceX = _lastRaytraceY = -1;
}

static bool shouldGetMerged(const scenegraph::SceneGraphNode &node, NodeMergeFlags flags) {
	bool add = false;
	if ((flags & NodeMergeFlags::Visible) == NodeMergeFlags::Visible) {
		add = node.visible();
	} else if ((flags & NodeMergeFlags::Invisible) == NodeMergeFlags::Invisible) {
		add = !node.visible();
	} else if ((flags & NodeMergeFlags::Locked) == NodeMergeFlags::Locked) {
		add = node.locked();
	} else if ((flags & NodeMergeFlags::All) == NodeMergeFlags::All) {
		add = true;
	}
	return add;
}

int SceneManager::mergeNodes(const core::DynamicArray<int>& nodeIds) {
	scenegraph::SceneGraph newSceneGraph;
	for (int nodeId : nodeIds) {
		scenegraph::SceneGraphNode copiedNode;
		const scenegraph::SceneGraphNode *node = sceneGraphModelNode(nodeId);
		if (node == nullptr) {
			continue;
		}
		scenegraph::copyNode(*node, copiedNode, true);
		newSceneGraph.emplace(core::move(copiedNode));
	}
	scenegraph::SceneGraph::MergedVolumePalette merged = newSceneGraph.merge();
	if (merged.first == nullptr) {
		return InvalidNodeId;
	}

	scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
	int parent = 0;
	if (scenegraph::SceneGraphNode* firstNode = sceneGraphNode(nodeIds.front())) {
		scenegraph::copyNode(*firstNode, newNode, false);
	}
	scenegraph::SceneGraphTransform &transform = newNode.keyFrame(0).transform();
	transform.setWorldTranslation(glm::vec3(0.0f));
	newNode.setVolume(merged.first, true);
	newNode.setPalette(merged.second);

	int newNodeId = moveNodeToSceneGraph(newNode, parent);
	if (newNodeId == InvalidNodeId) {
		return newNodeId;
	}
	for (int nodeId : nodeIds) {
		nodeRemove(nodeId, false);
	}
	return newNodeId;
}

int SceneManager::mergeNodes(NodeMergeFlags flags) {
	core::DynamicArray<int> nodeIds;
	nodeIds.reserve(_sceneGraph.size());
	for (auto iter = _sceneGraph.beginModel(); iter != _sceneGraph.end(); ++iter) {
		const scenegraph::SceneGraphNode &node = *iter;
		if (!shouldGetMerged(node, flags)) {
			continue;
		}
		nodeIds.push_back(node.id());
	}

	if (nodeIds.size() <= 1) {
		return InvalidNodeId;
	}

	return mergeNodes(nodeIds);
}

int SceneManager::mergeNodes(int nodeId1, int nodeId2) {
	if (!_sceneGraph.hasNode(nodeId1) || !_sceneGraph.hasNode(nodeId2)) {
		return InvalidNodeId;
	}
	voxel::RawVolume *volume1 = volume(nodeId1);
	if (volume1 == nullptr) {
		return InvalidNodeId;
	}
	voxel::RawVolume *volume2 = volume(nodeId2);
	if (volume2 == nullptr) {
		return InvalidNodeId;
	}
	core::DynamicArray<int> nodeIds(2);
	nodeIds[0] = nodeId1;
	nodeIds[1] = nodeId2;
	return mergeNodes(nodeIds);
}

void SceneManager::resetSceneState() {
	// this also resets the cursor voxel - but nodeActive() will set it to the first usable index
	// that's why this call must happen before the nodeActive() call.
	_modifierFacade.reset();
	scenegraph::SceneGraphNode &node = *_sceneGraph.beginModel();
	nodeActivate(node.id());
	_mementoHandler.clearStates();
	Log::debug("New volume for node %i", node.id());
	for (const auto &n : _sceneGraph.nodes()) {
		if (!n->second.isAnyModelNode()) {
			continue;
		}
		_mementoHandler.markInitialNodeState(n->second);
	}
	_dirty = false;
	_result = voxelutil::PickResult();
	_modifierFacade.setCursorVoxel(voxel::createVoxel(node.palette(), 0));
	setCursorPosition(cursorPosition(), true);
	setReferencePosition(node.region().getCenter());
	resetLastTrace();
}

void SceneManager::onNewNodeAdded(int newNodeId, bool isChildren) {
	if (newNodeId == InvalidNodeId) {
		return;
	}

	if (!isChildren) {
		_sceneGraph.updateTransforms();
	}

	if (scenegraph::SceneGraphNode *node = sceneGraphNode(newNodeId)) {
		const core::String &name = node->name();
		const scenegraph::SceneGraphNodeType type = node->type();
		Log::debug("Adding node %i with name %s", newNodeId, name.c_str());

		_mementoHandler.markNodeAdded(*node);

		for (int childId : node->children()) {
			onNewNodeAdded(childId, true);
		}

		markDirty();

		Log::debug("Add node %i to scene graph", newNodeId);
		if (type == scenegraph::SceneGraphNodeType::Model) {
			const voxel::Region &region = node->region();
			// update the whole volume
			_sceneRenderer->updateNodeRegion(newNodeId, region);

			_result = voxelutil::PickResult();
			if (!isChildren) {
				nodeActivate(newNodeId);
			}
		}
	}
}

int SceneManager::moveNodeToSceneGraph(scenegraph::SceneGraphNode &node, int parent) {
	const int newNodeId = scenegraph::moveNodeToSceneGraph(_sceneGraph, node, parent, false);
	onNewNodeAdded(newNodeId, false);
	return newNodeId;
}

bool SceneManager::loadSceneGraph(scenegraph::SceneGraph&& sceneGraph) {
	core_trace_scoped(LoadSceneGraph);
	bool createDiff = core::Var::get("ve_diff", "false")->boolVal();
	if (createDiff) {
		for (const auto &entry : sceneGraph.nodes()) {
			const scenegraph::SceneGraphNode &node = entry->second;
			if (!node.isModelNode()) {
				continue;
			}

			scenegraph::SceneGraphNode *existingNode = _sceneGraph.findNodeByName(node.name());
			if (existingNode == nullptr) {
				const int activeNode = _sceneGraph.activeNode();
				existingNode = &_sceneGraph.node(activeNode);
			}

			voxel::RawVolume *v = voxelutil::diffVolumes(existingNode->volume(), node.volume());
			if (v == nullptr) {
				Log::info("No diff between volumes of node %s", node.name().c_str());
				continue;
			}
			scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
			newNode.setVolume(v, true);
			newNode.setName("Diff " + node.name());
			_sceneGraph.emplace(core::move(newNode), existingNode->id());
		}
		return true;
	}

	_sceneGraph = core::move(sceneGraph);
	_sceneRenderer->clear();

	const size_t nodesAdded = _sceneGraph.size();
	if (nodesAdded == 0) {
		if (_sceneGraph.empty(scenegraph::SceneGraphNodeType::Point)) {
			Log::warn("Failed to load any model volumes");
			const voxel::Region region(glm::ivec3(0), glm::ivec3(size() - 1));
			newScene(true, "", region);
			return false;
		} else {
			// only found points, let's create a new model node to please the editor
			addModelChild("", size(), size(), size());
		}
	}
	resetSceneState();
	return true;
}

bool SceneManager::splitVolumes() {
	scenegraph::SceneGraph newSceneGraph;
	if (scenegraph::splitVolumes(_sceneGraph, newSceneGraph, false, false)) {
		return loadSceneGraph(core::move(newSceneGraph));
	}

	scenegraph::SceneGraph newSceneGraph2;
	if (scenegraph::splitVolumes(_sceneGraph, newSceneGraph2, false, true)) {
		return loadSceneGraph(core::move(newSceneGraph2));
	}
	return false;
}

void SceneManager::updateGridRenderer(const voxel::Region& region) {
	_sceneRenderer->updateGridRegion(region);
}

scenegraph::SceneGraphNode *SceneManager::sceneGraphModelNode(int nodeId) {
	scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId);
	if (node == nullptr || node->type() != scenegraph::SceneGraphNodeType::Model) {
		return nullptr;
	}
	return node;
}

scenegraph::SceneGraphNode *SceneManager::sceneGraphNode(int nodeId) {
	if (_sceneGraph.hasNode(nodeId)) {
		return &_sceneGraph.node(nodeId);
	}
	return nullptr;
}

const scenegraph::SceneGraphNode *SceneManager::sceneGraphNode(int nodeId) const {
	if (_sceneGraph.hasNode(nodeId)) {
		return &_sceneGraph.node(nodeId);
	}
	return nullptr;
}

const scenegraph::SceneGraph &SceneManager::sceneGraph() const {
	return _sceneGraph;
}

scenegraph::SceneGraph &SceneManager::sceneGraph() {
	return _sceneGraph;
}

bool SceneManager::setAnimation(const core::String &animation) {
	return _sceneGraph.setAnimation(animation);
}

bool SceneManager::addAnimation(const core::String &animation) {
	if (_sceneGraph.addAnimation(animation)) {
		// TODO: memento
		//_mementoHandler.markAddedAnimation(animation);
		return true;
	}
	return false;
}

bool SceneManager::duplicateAnimation(const core::String &animation, const core::String &newName) {
	if (_sceneGraph.duplicateAnimation(animation, newName)) {
		// TODO: memento
		//_mementoHandler.markAddedAnimation(animation);
		return true;
	}
	return false;
}

bool SceneManager::removeAnimation(const core::String &animation) {
	if (_sceneGraph.removeAnimation(animation)) {
		// TODO: memento
		//_mementoHandler.markRemovedAnimation(animation);
		return true;
	}
	return false;
}

bool SceneManager::setNewVolume(int nodeId, voxel::RawVolume* volume, bool deleteMesh) {
	core_trace_scoped(SetNewVolume);
	scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return false;
	}
	return setSceneGraphNodeVolume(*node, volume);
}

bool SceneManager::setSceneGraphNodeVolume(scenegraph::SceneGraphNode &node, voxel::RawVolume* volume) {
	if (node.type() != scenegraph::SceneGraphNodeType::Model) {
		return false;
	}
	if (node.volume() == volume) {
		return true;
	}

	node.setVolume(volume, true);
	// the old volume pointer might no longer be used
	_sceneRenderer->removeNode(node.id());

	const voxel::Region& region = volume->region();
	updateGridRenderer(region);

	_dirty = false;
	_result = voxelutil::PickResult();
	setCursorPosition(cursorPosition(), true);
	setReferencePosition(region.getLowerCenter());
	resetLastTrace();
	return true;
}

bool SceneManager::newScene(bool force, const core::String &name, voxel::RawVolume *v) {
	_sceneGraph.clear();
	_sceneRenderer->clear();

	scenegraph::SceneGraphNode newNode;
	newNode.setVolume(v, true);
	if (name.empty()) {
		newNode.setName("unnamed");
	} else {
		newNode.setName(name);
	}
	const int nodeId = scenegraph::moveNodeToSceneGraph(_sceneGraph, newNode, 0);
	if (nodeId == InvalidNodeId) {
		Log::error("Failed to add empty volume to new scene graph");
		return false;
	}
	setReferencePosition(v->region().getLowerCenter());
	resetSceneState();
	_lastFilename.clear();
	return true;
}

bool SceneManager::newScene(bool force, const core::String& name, const voxel::Region& region) {
	if (dirty() && !force) {
		return false;
	}
	voxel::RawVolume* v = new voxel::RawVolume(region);
	return newScene(force, name, v);
}

void SceneManager::rotate(math::Axis axis) {
	_sceneGraph.foreachGroup([&](int nodeId) {
		scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId);
		if (node == nullptr) {
			return;
		}
		const voxel::RawVolume *v = node->volume();
		if (v == nullptr) {
			return;
		}
		voxel::RawVolume *newVolume = voxelutil::rotateAxis(v, axis);
		if (newVolume == nullptr) {
			return;
		}
		voxel::Region r = newVolume->region();
		r.accumulate(v->region());
		setSceneGraphNodeVolume(*node, newVolume);
		modified(nodeId, r);
	});
}

void SceneManager::nodeMoveVoxels(int nodeId, const glm::ivec3& m) {
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return;
	}

	// TODO: only move the selected voxels
	v->move(m);
	modified(nodeId, v->region());
}

void SceneManager::move(int x, int y, int z) {
	const glm::ivec3 v(x, y, z);
	_sceneGraph.foreachGroup([&] (int nodeId) {
		nodeMoveVoxels(nodeId, v);
	});
}

void SceneManager::nodeShift(int nodeId, const glm::ivec3& m) {
	scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return;
	}
	voxel::RawVolume *v = node->volume();
	if (v == nullptr) {
		return;
	}
	voxel::Region region = v->region();
	v->translate(m);
	region.accumulate(v->region());
	modified(nodeId, region);
}

void SceneManager::shift(int x, int y, int z) {
	const glm::ivec3 v(x, y, z);
	_sceneGraph.foreachGroup([&] (int nodeId) {
		nodeShift(nodeId, v);
	});
}

bool SceneManager::setGridResolution(int resolution) {
	if (_modifierFacade.gridResolution() == resolution) {
		return false;
	}
	_modifierFacade.setGridResolution(resolution);
	setCursorPosition(cursorPosition(), true);
	return true;
}

void SceneManager::render(voxelrender::RenderContext &renderContext, const video::Camera& camera, uint8_t renderMask) {
	renderContext.frameBuffer.bind(true);
	_sceneRenderer->updateLockedPlanes(_modifierFacade.lockedAxis(), _sceneGraph, cursorPosition());

	renderContext.frame = _currentFrameIdx;
	renderContext.sceneGraph = &_sceneGraph;

	const bool renderScene = (renderMask & RenderScene) != 0u;
	if (renderScene) {
		_sceneRenderer->renderScene(renderContext, camera);
	}
	const bool renderUI = (renderMask & RenderUI) != 0u;
	if (renderUI) {
		_sceneRenderer->renderUI(renderContext, camera);
		if (!renderContext.sceneMode) {
			_modifierFacade.render(camera, activePalette());
		}
	}
	renderContext.frameBuffer.unbind();
}

void SceneManager::construct() {
	_modifierFacade.construct();
	_mementoHandler.construct();
	_sceneRenderer->construct();
	_movement.construct();

	_autoSaveSecondsDelay = core::Var::get(cfg::VoxEditAutoSaveSeconds, "180", -1, "Delay in second between autosaves - 0 disables autosaves");
	_movementSpeed = core::Var::get(cfg::VoxEditMovementSpeed, "180.0f");
	_transformUpdateChildren = core::Var::get(cfg::VoxEditTransformUpdateChildren, "true", -1, "Update the children of a node when the transform of the node changes");

	command::Command::registerCommand("resizetoselection", [&](const command::CmdArgs &args) {
		const voxel::Region &region = accumulate(modifier().selections());
		nodeResize(sceneGraph().activeNode(), region);
	}).setHelp(_("Resize the volume to the current selection"));

	command::Command::registerCommand("xs", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::error("Usage: xs <lua-generator-script-filename> [help]");
			return;
		}
		const core::String luaCode = _luaApi.load(args[0]);
		if (luaCode.empty()) {
			Log::error("Failed to load %s", args[0].c_str());
			return;
		}

		core::DynamicArray<core::String> luaArgs;
		for (size_t i = 1; i < args.size(); ++i) {
			luaArgs.push_back(args[i]);
		}

		if (!runScript(luaCode, luaArgs)) {
			Log::error("Failed to execute %s", args[0].c_str());
		} else {
			Log::info("Executed script %s", args[0].c_str());
		}
	}).setHelp(_("Executes a lua script"))
		.setArgumentCompleter(voxelgenerator::scriptCompleter(_filesystem));

	for (int i = 0; i < lengthof(DIRECTIONS); ++i) {
		command::Command::registerActionButton(
				core::string::format("movecursor%s", DIRECTIONS[i].postfix),
				_move[i], "Move the cursor by keys, not but viewport mouse trace");
	}
	command::Command::registerCommand("palette_changeintensity", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Usage: palette_changeintensity [value]");
			return;
		}
		const float scale = core::string::toFloat(args[0]);
		const int nodeId = activeNode();
		scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
		palette::Palette &pal = node.palette();
		pal.changeIntensity(scale);
		_mementoHandler.markPaletteChange(node);
	}).setHelp(_("Change intensity by scaling the rgb values of the palette"));

	command::Command::registerCommand("palette_removeunused", [&] (const command::CmdArgs& args) {
		const bool updateVoxels = args.empty() ? false : core::string::toBool(args[0]);
		const int nodeId = activeNode();
		nodeRemoveUnusedColors(nodeId, updateVoxels);
	}).setHelp(_("Remove unused colors from palette"));

	command::Command::registerCommand("palette_sort", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Usage: palette_sort [hue|saturation|brightness|cielab|original]");
			return;
		}
		const core::String &type = args[0];
		const int nodeId = activeNode();
		scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
		palette::Palette &pal = node.palette();
		if (type == "hue") {
			pal.sortHue();
		} else if (type == "brightness") {
			pal.sortBrightness();
		} else if (type == "cielab") {
			pal.sortCIELab();
		} else if (type == "saturation") {
			pal.sortSaturation();
		} else if (type == "original") {
			pal.sortOriginal();
		}
		_mementoHandler.markPaletteChange(node);
	}).setHelp(_("Change intensity by scaling the rgb values of the palette")).
		setArgumentCompleter(command::valueCompleter({"hue", "saturation", "brightness", "cielab", "original"}));

	command::Command::registerActionButton("zoom_in", _zoomIn, "Zoom in");
	command::Command::registerActionButton("zoom_out", _zoomOut, "Zoom out");
	command::Command::registerActionButton("camera_rotate", _rotate, "Rotate the camera");
	command::Command::registerActionButton("camera_pan", _pan, "Pan the camera");
	command::Command::registerCommand("mouse_node_select", [&] (const command::CmdArgs&) {
		const int nodeId = traceScene();
		if (nodeId != InvalidNodeId) {
			Log::debug("switch active node to hovered from scene graph mode: %i", nodeId);
			nodeActivate(nodeId);
		}
	}).setHelp(_("Switch active node to hovered from scene graph mode"));

	command::Command::registerCommand("select", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Usage: select [all|none|invert]");
			return;
		}
		if (args[0] == "none") {
			_modifierFacade.unselect();
		} else if (args[0] == "all") {
			if (const scenegraph::SceneGraphNode *node = sceneGraphNode(activeNode())) {
				const voxel::Region &region = node->region();
				if (region.isValid()) {
					_modifierFacade.select(region.getLowerCorner(), region.getUpperCorner());
				}
			}
		} else if (args[0] == "invert") {
			if (const scenegraph::SceneGraphNode *node = sceneGraphNode(activeNode())) {
				_modifierFacade.invert(node->region());
			}
		}
	}).setHelp(_("Select all nothing or invert")).setArgumentCompleter(command::valueCompleter({"all", "none", "invert"}));

	command::Command::registerCommand("presentation", [] (const command::CmdArgs& args) {
		command::Command::execute("hideall; animate 2000 true true");
	}).setHelp(_("Cycle through all scene objects"));

	command::Command::registerCommand("align", [this] (const command::CmdArgs& args) {
		_sceneGraph.align();
		for (const auto &entry : _sceneGraph.nodes()) {
			const scenegraph::SceneGraphNode &node = entry->second;
			if (!node.isModelNode()) {
				continue;
			}
			modified(node.id(), _sceneGraph.resolveRegion(node), true);
		}
	}).setHelp(_("Allow to align all nodes on the floor next to each other without overlapping"));

	command::Command::registerCommand("modelssave", [&] (const command::CmdArgs& args) {
		core::String dir = ".";
		if (!args.empty()) {
			dir = args[0];
		}
		if (!saveModels(dir)) {
			Log::error("Failed to save models to dir: %s", dir.c_str());
		}
	}).setHelp(_("Save all model nodes into filenames represented by their node names"));

	command::Command::registerCommand("modelsave", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc < 1) {
			Log::info("Usage: modelsave <nodeid> [<file>]");
			return;
		}
		const int nodeId = core::string::toInt(args[0]);
		core::String file = core::string::format("node%i.vengi", nodeId);
		if (args.size() == 2) {
			file = args[1];
		}
		if (!saveNode(nodeId, file)) {
			Log::error("Failed to save node %i to file: %s", nodeId, file.c_str());
		}
	}).setHelp(_("Save a single node to the given path with their node names")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("newscene", [&] (const command::CmdArgs& args) {
		const char *name = args.size() > 0 ? args[0].c_str() : "";
		const char *width = args.size() > 1 ? args[1].c_str() : "64";
		const char *height = args.size() > 2 ? args[2].c_str() : width;
		const char *depth = args.size() > 3 ? args[3].c_str() : height;
		const int iw = core::string::toInt(width) - 1;
		const int ih = core::string::toInt(height) - 1;
		const int id = core::string::toInt(depth) - 1;
		const voxel::Region region(glm::zero<glm::ivec3>(), glm::ivec3(iw, ih, id));
		if (!region.isValid()) {
			Log::warn("Invalid size provided (%i:%i:%i)", iw, ih, id);
			return;
		}
		if (!newScene(true, name, region)) {
			Log::warn("Could not create new scene");
		}
	}).setHelp(_("Create a new scene (with a given name and width, height, depth - all optional)"));

	command::Command::registerCommand("crop", [&] (const command::CmdArgs& args) {
		crop();
	}).setHelp(_("Crop the current active node to the voxel boundaries"));

	command::Command::registerCommand("splitobjects", [&] (const command::CmdArgs& args) {
		splitObjects();
	}).setHelp(_("Split the current active node into multiple nodes"));

	command::Command::registerCommand("scaledown", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		int nodeId = activeNode();
		if (argc == 1) {
			nodeId = core::string::toInt(args[0]);
		}
		scaleDown(nodeId);
	}).setHelp(_("Scale the current active node or the given node down")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("scaleup", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		int nodeId = activeNode();
		if (argc == 1) {
			nodeId = core::string::toInt(args[0]);
		}
		scaleUp(nodeId);
	}).setHelp(_("Scale the current active node or the given node up")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("colortomodel", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc < 1) {
			const voxel::Voxel voxel = _modifierFacade.cursorVoxel();
			colorToNewNode(voxel);
		} else {
			const uint8_t index = core::string::toInt(args[0]);
			const voxel::Voxel voxel = voxel::createVoxel(activePalette(), index);
			colorToNewNode(voxel);
		}
	}).setHelp(_("Move the voxels of the current selected palette index or the given index into a new node"));

	command::Command::registerCommand("abortaction", [&] (const command::CmdArgs& args) {
		_modifierFacade.stop();
	}).setHelp(_("Aborts the current modifier action"));

	command::Command::registerCommand("fillhollow", [&] (const command::CmdArgs& args) {
		fillHollow();
	}).setHelp(_("Fill the inner parts of closed models"));

	command::Command::registerCommand("hollow", [&] (const command::CmdArgs& args) {
		hollow();
	}).setHelp(_("Remove non visible voxels"));

	command::Command::registerCommand("fill", [&] (const command::CmdArgs& args) {
		fill();
	}).setHelp(_("Fill voxels in the current selection"));

	command::Command::registerCommand("clear", [&] (const command::CmdArgs& args) {
		clear();
	}).setHelp(_("Remove all voxels in the current selection"));

	command::Command::registerCommand("setreferenceposition", [&] (const command::CmdArgs& args) {
		if (args.size() != 3) {
			Log::info("Expected to get x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		setReferencePosition(glm::ivec3(x, y, z));
	}).setHelp(_("Set the reference position to the specified position"));

	command::Command::registerCommand("movecursor", [this] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Expected to get relative x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		moveCursor(x, y, z);
	}).setHelp(_("Move the cursor by the specified offsets"));

	command::Command::registerCommand("loadpalette", [this] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Expected to get the palette NAME as part of palette-NAME.[png|lua]");
			return;
		}
		bool searchBestColors = false;
		loadPalette(args[0], searchBestColors, true);
	}).setHelp(_("Load a palette by name. E.g. 'built-in:nippon' or 'lospec:id'")).setArgumentCompleter(paletteCompleter());

	command::Command::registerCommand("cursor", [this] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Expected to get x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		setCursorPosition(glm::ivec3(x, y, z), true);
		_traceViaMouse = false;
	}).setHelp(_("Set the cursor to the specified position"));

	command::Command::registerCommand("setreferencepositiontocursor", [&] (const command::CmdArgs& args) {
		setReferencePosition(cursorPosition());
	}).setHelp(_("Set the reference position to the current cursor position"));

	command::Command::registerCommand("resize", [this] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc == 1) {
			const int size = core::string::toInt(args[0]);
			resizeAll(glm::ivec3(size));
		} else if (argc == 3) {
			glm::ivec3 size;
			for (int i = 0; i < argc; ++i) {
				size[i] = core::string::toInt(args[i]);
			}
			resizeAll(size);
		} else {
			resizeAll(glm::ivec3(1));
		}
	}).setHelp(_("Resize your volume about given x, y and z size"));

	command::Command::registerCommand("modelsize", [this] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc == 1) {
			const int size = core::string::toInt(args[0]);
			nodeResize(activeNode(), glm::ivec3(size));
		} else if (argc == 3) {
			glm::ivec3 size;
			for (int i = 0; i < argc; ++i) {
				size[i] = core::string::toInt(args[i]);
			}
			nodeResize(activeNode(), size);
		} else {
			nodeResize(activeNode(), glm::ivec3(1));
		}
	}).setHelp(_("Resize your current model node about given x, y and z size"));

	command::Command::registerCommand("shift", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc != 3) {
			Log::info("Expected to get x, y and z values");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		shift(x, y, z);
	}).setHelp(_("Shift the volume by the given values"));

	command::Command::registerCommand("center_referenceposition", [&] (const command::CmdArgs& args) {
		const glm::ivec3& refPos = referencePosition();
		_sceneGraph.foreachGroup([&](int nodeId) {
			const voxel::RawVolume *v = volume(nodeId);
			if (v == nullptr) {
				return;
			}
			const voxel::Region& region = v->region();
			const glm::ivec3& center = region.getCenter();
			const glm::ivec3& delta = refPos - center;
			nodeShift(nodeId, delta);
		});
	}).setHelp(_("Center the current active nodes at the reference position"));

	command::Command::registerCommand("center_origin", [&](const command::CmdArgs &args) {
		_sceneGraph.foreachGroup([&](int nodeId) {
			const voxel::RawVolume *v = volume(nodeId);
			if (v == nullptr) {
				return;
			}
			const voxel::Region& region = v->region();
			const glm::ivec3& delta = -region.getCenter();
			nodeShift(nodeId, delta);
		});
		setReferencePosition(glm::zero<glm::ivec3>());
	}).setHelp(_("Center the current active nodes at the origin"));

	command::Command::registerCommand("move", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc != 3) {
			Log::info("Expected to get x, y and z values");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		move(x, y, z);
	}).setHelp(_("Move the voxels inside the volume by the given values without changing the volume bounds"));

	command::Command::registerCommand("copy", [&] (const command::CmdArgs& args) {
		copy();
	}).setHelp(_("Copy selection"));

	command::Command::registerCommand("paste", [&] (const command::CmdArgs& args) {
		const Selections& selections = _modifierFacade.selections();
		if (!selections.empty()) {
			voxel::Region r = selections[0];
			for (const Selection &region : selections) {
				r.accumulate(region);
			}
			paste(r.getLowerCorner());
		} else {
			paste(referencePosition());
		}
	}).setHelp(_("Paste clipboard to current selection or reference position"));

	command::Command::registerCommand("pastecursor", [&] (const command::CmdArgs& args) {
		paste(_modifierFacade.cursorPosition());
	}).setHelp(_("Paste clipboard to current cursor position"));

	command::Command::registerCommand("pastenewnode", [&] (const command::CmdArgs& args) {
		pasteAsNewNode();
	}).setHelp(_("Paste clipboard as a new node"));

	command::Command::registerCommand("cut", [&] (const command::CmdArgs& args) {
		cut();
	}).setHelp(_("Cut selection"));

	command::Command::registerCommand("undo", [&] (const command::CmdArgs& args) {
		undo();
	}).setHelp(_("Undo your last step"));

	command::Command::registerCommand("redo", [&] (const command::CmdArgs& args) {
		redo();
	}).setHelp(_("Redo your last step"));

	command::Command::registerCommand("rotate", [&] (const command::CmdArgs& args) {
		if (args.size() < 1) {
			Log::info("Usage: rotate <x|y|z>");
			return;
		}
		const math::Axis axis = math::toAxis(args[0]);
		rotate(axis);
	}).setHelp(_("Rotate active nodes around the given axis"));

	command::Command::registerCommand("modelmerge", [&] (const command::CmdArgs& args) {
		int nodeId1;
		int nodeId2;
		if (args.size() == 1) {
			if (args[0] == "all") {
				scenegraph::SceneGraph::MergedVolumePalette merged = _sceneGraph.merge();
				newScene(true, "merged", merged.first);
				if (auto *node = _sceneGraph.firstModelNode()) {
					node->setPalette(merged.second);
				}
				return;
			}
			nodeId2 = core::string::toInt(args[0]);
			nodeId1 = _sceneGraph.prevModelNode(nodeId2);
		} else if (args.size() == 2) {
			nodeId1 = core::string::toInt(args[0]);
			nodeId2 = core::string::toInt(args[1]);
		} else {
			nodeId2 = activeNode();
			nodeId1 = _sceneGraph.prevModelNode(nodeId2);
		}
		mergeNodes(nodeId1, nodeId2);
	}).setHelp(_("Merge two given nodes or active model node with the next one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("modelmergeall", [&] (const command::CmdArgs& args) {
		mergeNodes(NodeMergeFlags::All);
	}).setHelp(_("Merge all nodes"));

	command::Command::registerCommand("modelmergevisible", [&] (const command::CmdArgs& args) {
		mergeNodes(NodeMergeFlags::Visible);
	}).setHelp(_("Merge all visible nodes"));

	command::Command::registerCommand("modelmergelocked", [&] (const command::CmdArgs& args) {
		mergeNodes(NodeMergeFlags::Locked);
	}).setHelp(_("Merge all locked nodes"));

	command::Command::registerCommand("animate", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Usage: animate <nodedelaymillis> <0|1> <resetcamera>");
			Log::info("nodedelay of 0 will stop the animation, too");
			return;
		}
		if (args.size() == 2) {
			_animationResetCamera = false;
			if (!core::string::toBool(args[1])) {
				_animationSpeed = 0.0;
				return;
			}
		} else if (args.size() == 3) {
			_animationResetCamera = core::string::toBool(args[2]);
		}
		_animationSpeed = core::string::toDouble(args[0]) / 1000.0;
	}).setHelp(_("Animate all nodes with the given delay in millis between the frames"));

	command::Command::registerCommand("setcolor", [&] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: setcolor <index>");
			return;
		}
		const uint8_t index = core::string::toInt(args[0]);
		const voxel::Voxel voxel = voxel::createVoxel(activePalette(), index);
		_modifierFacade.setCursorVoxel(voxel);
	}).setHelp(_("Use the given index to select the color from the current palette"));

	command::Command::registerCommand("setcolorrgb", [&] (const command::CmdArgs& args) {
		if (args.size() != 3) {
			Log::info("Usage: setcolorrgb <red> <green> <blue> (color range 0-255)");
			return;
		}
		const uint8_t red = core::string::toInt(args[0]);
		const uint8_t green = core::string::toInt(args[1]);
		const uint8_t blue = core::string::toInt(args[2]);
		const core::RGBA color(red, green, blue);
		const int index = activePalette().getClosestMatch(color);
		const voxel::Voxel voxel = voxel::createVoxel(activePalette(), index);
		_modifierFacade.setCursorVoxel(voxel);
	}).setHelp(_("Set the current selected color by finding the closest rgb match in the palette"));

	command::Command::registerCommand("pickcolor", [&] (const command::CmdArgs& args) {
		// during mouse movement, the current cursor position might be at an air voxel (this
		// depends on the mode you are editing in), thus we should use the cursor voxel in
		// that case
		if (_traceViaMouse && !voxel::isAir(hitCursorVoxel().getMaterial())) {
			_modifierFacade.setCursorVoxel(hitCursorVoxel());
			return;
		}
		// resolve the voxel via cursor position. This allows to use also get the proper
		// result if we moved the cursor via keys (and thus might have skipped tracing)
		const glm::ivec3& cursorPos = _modifierFacade.cursorPosition();
		if (const voxel::RawVolume *v = activeVolume()) {
			const voxel::Voxel& voxel = v->voxel(cursorPos);
			_modifierFacade.setCursorVoxel(voxel);
		}
	}).setHelp(_("Pick the current selected color from current cursor voxel"));

	command::Command::registerCommand("flip", [&] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: flip <x|y|z>");
			return;
		}
		const math::Axis axis = math::toAxis(args[0]);
		flip(axis);
	}).setHelp(_("Flip the selected nodes around the given axis")).setArgumentCompleter(command::valueCompleter({"x", "y", "z"}));

	command::Command::registerCommand("modeladd", [&] (const command::CmdArgs& args) {
		const char *name = args.size() > 0 ? args[0].c_str() : "";
		const char *width = args.size() > 1 ? args[1].c_str() : "64";
		const char *height = args.size() > 2 ? args[2].c_str() : width;
		const char *depth = args.size() > 3 ? args[3].c_str() : height;
		const int iw = core::string::toInt(width);
		const int ih = core::string::toInt(height);
		const int id = core::string::toInt(depth);
		addModelChild(name, iw, ih, id);
	}).setHelp(_("Add a new model node (with a given name and width, height, depth - all optional)"));

	command::Command::registerCommand("nodedelete", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			nodeRemove(*node, false);
		}
	}).setHelp(_("Delete a particular node by id - or the current active one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodelock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(true);
		}
	}).setHelp(_("Lock a particular node by id - or the current active one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodetogglelock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(!node->locked());
		}
	}).setHelp(_("Toggle the lock state of a particular node by id - or the current active one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodeunlock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(false);
		}
	}).setHelp(_("Unlock a particular node by id - or the current active one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodeactivate", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Active node: %i", activeNode());
			return;
		}
		const int nodeId = core::string::toInt(args[0]);
		nodeActivate(nodeId);
	}).setHelp(_("Set or print the current active node")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodetogglevisible", [&](const command::CmdArgs &args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			node->setVisible(!node->visible());
		}
	}).setHelp(_("Toggle the visible state of a node")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("showall", [&] (const command::CmdArgs& args) {
		for (auto iter = _sceneGraph.beginAll(); iter != _sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &node = *iter;
			node.setVisible(true);
		}
	}).setHelp(_("Show all nodes"));

	command::Command::registerCommand("hideall", [&](const command::CmdArgs &args) {
		for (auto iter = _sceneGraph.beginAll(); iter != _sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &node = *iter;
			node.setVisible(false);
		}
	}).setHelp(_("Hide all nodes"));

	command::Command::registerCommand("nodeshowallchildren", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		_sceneGraph.visitChildren(nodeId, true, [] (scenegraph::SceneGraphNode &node) {
			node.setVisible(true);
		});
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			node->setVisible(true);
		}
	}).setHelp(_("Show all children nodes"));

	command::Command::registerCommand("nodehideallchildren", [&](const command::CmdArgs &args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		_sceneGraph.visitChildren(nodeId, true, [] (scenegraph::SceneGraphNode &node) {
			node.setVisible(false);
		});
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			node->setVisible(false);
		}
	}).setHelp(_("Hide all children nodes"));

	command::Command::registerCommand("nodehideothers", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		for (auto iter = _sceneGraph.beginAll(); iter != _sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &node = *iter;
			if (node.id() == nodeId) {
				node.setVisible(true);
				continue;
			}
			node.setVisible(false);
		}
	}).setHelp(_("Hide all model nodes except the active one")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("modellockall", [&](const command::CmdArgs &args) {
		for (auto iter = _sceneGraph.beginModel(); iter != _sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &node = *iter;
			node.setLocked(true);
		}
	}).setHelp(_("Lock all nodes"));

	command::Command::registerCommand("modelunlockall", [&] (const command::CmdArgs& args) {
		for (auto iter = _sceneGraph.beginModel(); iter != _sceneGraph.end(); ++iter) {
			scenegraph::SceneGraphNode &node = *iter;
			node.setLocked(false);
		}
	}).setHelp(_("Unlock all nodes"));

	command::Command::registerCommand("noderename", [&] (const command::CmdArgs& args) {
		if (args.size() == 1) {
			const int nodeId = activeNode();
			if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
				nodeRename(*node, args[0]);
			}
		} else if (args.size() == 2) {
			const int nodeId = core::string::toInt(args[0]);
			if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
				nodeRename(*node, args[1]);
			}
		} else {
			Log::info("Usage: noderename [<nodeid>] newname");
		}
	}).setHelp(_("Rename the current node or the given node id")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("nodeduplicate", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			nodeDuplicate(*node);
		}
	}).setHelp(_("Duplicates the current node or the given node id")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("modelref", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			nodeReference(*node);
		}
	}).setHelp(_("Create a node reference for the given node id")).setArgumentCompleter(nodeCompleter(_sceneGraph));

	command::Command::registerCommand("modelunref", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			nodeUnreference(*node);
		}
	}).setHelp(_("Unreference from model and allow to edit the voxels for this node"));
}

void SceneManager::nodeRemoveUnusedColors(int nodeId, bool updateVoxels) {
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	voxel::RawVolume *v = node.volume();
	if (v == nullptr) {
		return;
	}
	core::Array<bool, palette::PaletteMaxColors> usedColors;
	usedColors.fill(false);

	palette::Palette &pal = node.palette();
	voxelutil::visitVolume(*v, [&usedColors] (int x, int y, int z, const voxel::Voxel& voxel) {
		usedColors[voxel.getColor()] = true;
		return true;
	});
	int unused = 0;
	for (size_t i = 0; i < palette::PaletteMaxColors; ++i) {
		if (!usedColors[i]) {
			++unused;
		}
	}
	if (unused >= palette::PaletteMaxColors) {
		Log::warn("Removing all colors from the palette is not allowed");
		return;
	}
	Log::debug("Unused colors: %i", unused);
	if (updateVoxels) {
		int newMappingPos = 0;
		core::Array<uint8_t, palette::PaletteMaxColors> newMapping;
		for (size_t i = 0; i < palette::PaletteMaxColors; ++i) {
			if (usedColors[i]) {
				newMapping[i] = newMappingPos++;
			}
		}
		palette::Palette newPalette;
		for (size_t i = 0; i < palette::PaletteMaxColors; ++i) {
			if (usedColors[i]) {
				newPalette.setColor(newMapping[i], pal.color(i));
				newPalette.setMaterial(newMapping[i], pal.material(i));
			}
		}
		core_assert(newPalette.colorCount() > 0);
		pal = newPalette;
		voxelutil::visitVolume(*v, [v, &newMapping, &pal] (int x, int y, int z, const voxel::Voxel& voxel) {
			v->setVoxel(x, y, z, voxel::createVoxel(pal, newMapping[voxel.getColor()]));
			return true;
		});
		pal.markDirty();
		pal.markSave();
		// TODO: memento group - finish implementation see https://github.com/vengi-voxel/vengi/issues/376
		// ScopedMementoGroup scopedMementoGroup(_mementoHandler);
		_mementoHandler.markPaletteChange(node);
		modified(nodeId, v->region());
	} else {
		for (size_t i = 0; i < pal.size(); ++i) {
			if (!usedColors[i]) {
				pal.setColor(i, core::RGBA(127, 127, 127, 255));
			}
		}
		pal.markDirty();
		pal.markSave();
		_mementoHandler.markPaletteChange(node);
	}
}

int SceneManager::addModelChild(const core::String& name, int width, int height, int depth) {
	const voxel::Region region(0, 0, 0, width - 1, height - 1, depth - 1);
	if (!region.isValid()) {
		Log::warn("Invalid size provided (%i:%i:%i)", width, height, depth);
		return InvalidNodeId;
	}
	scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
	newNode.setVolume(new voxel::RawVolume(region), true);
	newNode.setName(name);
	const int parentId = activeNode();
	const int nodeId = moveNodeToSceneGraph(newNode, parentId);
	return nodeId;
}

void SceneManager::flip(math::Axis axis) {
	_sceneGraph.foreachGroup([&](int nodeId) {
		voxel::RawVolume *v = volume(nodeId);
		if (v == nullptr) {
			return;
		}
		voxel::RawVolume* newVolume = voxelutil::mirrorAxis(v, axis);
		voxel::Region r = newVolume->region();
		r.accumulate(v->region());
		if (!setNewVolume(nodeId, newVolume)) {
			delete newVolume;
			return;
		}
		modified(nodeId, r);
	});
}

bool SceneManager::init() {
	++_initialized;
	if (_initialized > 1) {
		Log::debug("Already initialized");
		return true;
	}

	palette::Palette palette;
	if (!palette.load(core::Var::getSafe(cfg::VoxEditLastPalette)->strVal().c_str())) {
		palette = voxel::getPalette();
	}
	if (!_mementoHandler.init()) {
		Log::error("Failed to initialize the memento handler");
		return false;
	}
	if (!_sceneRenderer->init()) {
		Log::error("Failed to initialize the scene renderer");
		return false;
	}
	if (!_modifierFacade.init()) {
		Log::error("Failed to initialize the modifier");
		return false;
	}
	if (!_movement.init()) {
		Log::error("Failed to initialize the movement controller");
		return false;
	}
	if (!_luaApi.init()) {
		Log::error("Failed to initialize the lua generator");
		return false;
	}

	_gridSize = core::Var::getSafe(cfg::VoxEditGridsize);
	_lastAutoSave = _timeProvider->tickSeconds();

	_modifierFacade.setLockedAxis(math::Axis::None, true);
	return true;
}

bool SceneManager::runScript(const core::String& luaCode, const core::DynamicArray<core::String>& args) {
	voxel::Region dirtyRegion = voxel::Region::InvalidRegion;

	if (luaCode.empty()) {
		Log::warn("No script selected");
		return false;
	}
	const int nodeId = _sceneGraph.activeNode();
	const voxel::Region &region = _sceneGraph.resolveRegion(_sceneGraph.node(nodeId));
	if (!_luaApi.exec(luaCode, _sceneGraph, nodeId, region, _modifierFacade.cursorVoxel(), dirtyRegion, args)) {
		return false;
	}
	if (dirtyRegion.isValid()) {
		modified(activeNode(), dirtyRegion, true);
	}
	if (_sceneGraph.dirty()) {
		markDirty();
		_sceneRenderer->clear();
		_sceneGraph.markClean();
	}
	return true;
}

bool SceneManager::animateActive() const {
	return _animationSpeed > 0.0;
}

void SceneManager::animate(double nowSeconds) {
	if (!animateActive()) {
		return;
	}
	if (_nextFrameSwitch > nowSeconds) {
		return;
	}
	_nextFrameSwitch = nowSeconds + _animationSpeed;

	if (_currentAnimationNodeId == InvalidNodeId) {
		_currentAnimationNodeId = (*_sceneGraph.beginModel()).id();
	}

	scenegraph::SceneGraphNode &prev = _sceneGraph.node(_currentAnimationNodeId);
	if (prev.isAnyModelNode()) {
		prev.setVisible(false);
	}

	_currentAnimationNodeId = _sceneGraph.nextModelNode(_currentAnimationNodeId);
	if (_currentAnimationNodeId == InvalidNodeId) {
		if (const auto *node = _sceneGraph.firstModelNode()) {
			_currentAnimationNodeId = node->id();
		} else {
			_animationSpeed = 0.0f;
		}
	}
	scenegraph::SceneGraphNode &node = _sceneGraph.node(_currentAnimationNodeId);
	if (node.isAnyModelNode()) {
		node.setVisible(true);
	}
	if (_animationResetCamera) {
		command::Command::execute("resetcamera");
	}
}

void SceneManager::zoom(video::Camera& camera, float level) const {
	if (camera.rotationType() == video::CameraRotationType::Target) {
		camera.zoom(level);
	} else {
		// see Movement class
		const glm::quat& rot = glm::angleAxis(0.0f, glm::up());
		float speed = level * _movementSpeed->floatVal();
		speed *= (float)deltaSeconds();
		camera.move(rot * glm::vec3(0.0f, 0.0f, speed));
	}
}

bool SceneManager::isLoading() const {
	return _loadingFuture.valid();
}

bool SceneManager::update(double nowSeconds) {
	updateDelta(nowSeconds);
	bool loadedNewScene = false;
	if (_loadingFuture.valid()) {
		using namespace std::chrono_literals;
		std::future_status status = _loadingFuture.wait_for(1ms);
		if (status == std::future_status::ready) {
			if (loadSceneGraph(core::move(_loadingFuture.get()))) {
				_needAutoSave = false;
				_dirty = false;
				loadedNewScene = true;
			}
			_loadingFuture = std::future<scenegraph::SceneGraph>();
		}
	}

	_movement.update(nowSeconds);
	video::Camera *camera = activeCamera();
	if (camera != nullptr && camera->rotationType() == video::CameraRotationType::Eye) {
		const glm::vec3& moveDelta = _movement.moveDelta(_movementSpeed->floatVal());
		if (camera->move(moveDelta)) {
			const video::CameraRotationType r = camera->rotationType();
			camera->setRotationType(video::CameraRotationType::Eye);
			camera->update(0.0);
			camera->setRotationType(r);
		}
	}

	_modifierFacade.brushContext().fixedOrthoSideView = camera == nullptr ? false : camera->isOrthoAligned();
	_modifierFacade.update(nowSeconds);

	updateDirtyRendererStates();

	_sceneRenderer->update();
	setGridResolution(_gridSize->intVal());
	for (int i = 0; i < lengthof(DIRECTIONS); ++i) {
		if (!_move[i].pressed()) {
			continue;
		}
		_move[i].execute(nowSeconds, 0.125, [&] () {
			const Direction& dir = DIRECTIONS[i];
			moveCursor(dir.x, dir.y, dir.z);
		});
	}
	if (_zoomIn.pressed()) {
		_zoomIn.execute(nowSeconds, 0.02, [&] () {
			if (_camera != nullptr) {
				zoom(*_camera, 1.0f);
			}
		});
	} else if (_zoomOut.pressed()) {
		_zoomOut.execute(nowSeconds, 0.02, [&] () {
			if (_camera != nullptr) {
				zoom(*_camera, -1.0f);
			}
		});
	}

	animate(nowSeconds);
	autosave();
	return loadedNewScene;
}

void SceneManager::shutdown() {
	if (_initialized > 0) {
		--_initialized;
	}
	if (_initialized != 0) {
		return;
	}

	autosave();

	_sceneRenderer->shutdown();
	_sceneGraph.clear();
	_mementoHandler.clearStates();

	_movement.shutdown();
	_modifierFacade.shutdown();
	_mementoHandler.shutdown();
	_luaApi.shutdown();

	command::Command::unregisterActionButton("zoom_in");
	command::Command::unregisterActionButton("zoom_out");
	command::Command::unregisterActionButton("camera_rotate");
	command::Command::unregisterActionButton("camera_pan");
}

void SceneManager::lsystem(const core::String &axiom, const core::DynamicArray<voxelgenerator::lsystem::Rule> &rules, float angle, float length,
		float width, float widthIncrement, int iterations, float leavesRadius) {
	math::Random random;
	const int nodeId = activeNode();
	voxel::RawVolume *v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::RawVolumeWrapper wrapper(v);
	voxelgenerator::lsystem::generate(wrapper, referencePosition(), axiom, rules, angle, length, width, widthIncrement, iterations, random, leavesRadius);
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::createTree(const voxelgenerator::TreeContext& ctx) {
	math::Random random(ctx.cfg.seed);
	const int nodeId = activeNode();
	voxel::RawVolume *v = volume(nodeId);
	if (v == nullptr) {
		return;
	}
	voxel::RawVolumeWrapper wrapper(v);
	voxelgenerator::tree::createTree(wrapper, ctx, random);
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::setReferencePosition(const glm::ivec3& pos) {
	_modifierFacade.setReferencePosition(pos);
}

void SceneManager::moveCursor(int x, int y, int z) {
	glm::ivec3 p = cursorPosition();
	const int res = _modifierFacade.gridResolution();
	p.x += x * res;
	p.y += y * res;
	p.z += z * res;
	setCursorPosition(p, true);
	_traceViaMouse = false;
	if (const voxel::RawVolume *v = activeVolume()) {
		const voxel::Voxel &voxel = v->voxel(cursorPosition());
		_modifierFacade.setHitCursorVoxel(voxel);
	}
}

void SceneManager::setCursorPosition(glm::ivec3 pos, bool force) {
	const voxel::RawVolume* v = activeVolume();
	if (v == nullptr) {
		return;
	}

	const int res = _modifierFacade.gridResolution();
	const voxel::Region& region = v->region();
	const glm::ivec3& mins = region.getLowerCorner();
	const glm::ivec3 delta = pos - mins;
	if (delta.x % res != 0) {
		pos.x = mins.x + (delta.x / res) * res;
	}
	if (delta.y % res != 0) {
		pos.y = mins.y + (delta.y / res) * res;
	}
	if (delta.z % res != 0) {
		pos.z = mins.z + (delta.z / res) * res;
	}

	const math::Axis lockedAxis = _modifierFacade.lockedAxis();

	// make a copy here - no reference - otherwise the comparison below won't
	// do anything else than comparing the same values.
	const glm::ivec3 oldCursorPos = cursorPosition();
	if (!force) {
		if ((lockedAxis & math::Axis::X) != math::Axis::None) {
			pos.x = oldCursorPos.x;
		}
		if ((lockedAxis & math::Axis::Y) != math::Axis::None) {
			pos.y = oldCursorPos.y;
		}
		if ((lockedAxis & math::Axis::Z) != math::Axis::None) {
			pos.z = oldCursorPos.z;
		}
	}

	if (!region.containsPoint(pos)) {
		pos = region.moveInto(pos.x, pos.y, pos.z);
	}
	// TODO: multiple different viewports....
	_modifierFacade.setCursorPosition(pos, _result.hitFace);
	if (oldCursorPos == pos) {
		return;
	}
	_dirtyRenderer |= DirtyRendererLockedAxis;
}

void SceneManager::updateDirtyRendererStates() {
	if (_dirtyRenderer & DirtyRendererLockedAxis) {
		_dirtyRenderer &= ~DirtyRendererLockedAxis;
		_sceneRenderer->updateLockedPlanes(_modifierFacade.lockedAxis(), _sceneGraph, cursorPosition());
	}
	if (_dirtyRenderer & DirtyRendererGridRenderer) {
		_dirtyRenderer &= ~DirtyRendererGridRenderer;
		updateGridRenderer(_sceneGraph.node(activeNode()).region());
	}
}

bool SceneManager::trace(bool sceneMode, bool force) {
	if (_modifierFacade.isLocked()) {
		return false;
	}
	if (sceneMode) {
		return true;
	}

	return mouseRayTrace(force);
}

int SceneManager::traceScene() {
	const int previousNodeId = activeNode();
	int nodeId = InvalidNodeId;
	core_trace_scoped(EditorSceneOnProcessUpdateRay);
	float intersectDist = _camera->farPlane();
	const math::Ray& ray = _camera->mouseRay(_mouseCursor);
	for (auto entry : _sceneGraph.nodes()) {
		const scenegraph::SceneGraphNode& node = entry->second;
		if (previousNodeId == node.id()) {
			continue;
		}
		if (!node.isAnyModelNode()) {
			continue;
		}
		if (!node.visible()) {
			continue;
		}
		if (!_sceneRenderer->isVisible(node.id())) {
			continue;
		}
		float distance = 0.0f;
		const voxel::Region& region = _sceneGraph.resolveRegion(node);
		const glm::vec3 pivot = node.pivot();
		const scenegraph::FrameTransform &transform = _sceneGraph.transformForFrame(node, _currentFrameIdx);
		const math::OBB<float>& obb = toOBB(true, region, pivot, transform);
		if (obb.intersect(ray.origin, ray.direction, distance)) {
			if (distance < intersectDist) {
				intersectDist = distance;
				nodeId = node.id();
			}
		}
	}
	Log::trace("Hovered node: %i", nodeId);
	return nodeId;
}

void SceneManager::updateCursor() {
	if (_modifierFacade.modifierTypeRequiresExistingVoxel()) {
		if (_result.didHit) {
			setCursorPosition(_result.hitVoxel);
		} else if (_result.validPreviousPosition) {
			setCursorPosition(_result.previousPosition);
		}
	} else if (_result.validPreviousPosition) {
		setCursorPosition(_result.previousPosition);
	} else if (_result.didHit) {
		setCursorPosition(_result.hitVoxel);
	}

	const voxel::RawVolume *v = activeVolume();
	if (_result.didHit && v != nullptr) {
		_modifierFacade.setHitCursorVoxel(v->voxel(_result.hitVoxel));
	} else {
		_modifierFacade.setHitCursorVoxel(voxel::Voxel());
	}
	if (v) {
		_modifierFacade.setVoxelAtCursor(v->voxel(_modifierFacade.cursorPosition()));
	}
}

bool SceneManager::mouseRayTrace(bool force) {
	// mouse tracing is disabled - e.g. because the voxel cursor was moved by keyboard
	// shortcuts. In this case the execution of the modifier would result in a
	// re-execution of the trace. And that would move the voxel cursor to the mouse pos
	if (!_traceViaMouse) {
		return false;
	}
	// if the trace is not forced, and the mouse cursor position did not change, don't
	// re-execute the trace.
	if (_lastRaytraceX == _mouseCursor.x && _lastRaytraceY == _mouseCursor.y && !force) {
		return true;
	}
	const video::Camera *camera = activeCamera();
	if (camera == nullptr) {
		return false;
	}
	const voxel::RawVolume* v = activeVolume();
	if (v == nullptr) {
		return false;
	}
	const math::Ray& ray = camera->mouseRay(_mouseCursor);
	const float rayLength = camera->farPlane();

	const glm::vec3& dirWithLength = ray.direction * rayLength;
	static constexpr voxel::Voxel air;

	Log::trace("Execute new trace for %i:%i (%i:%i)",
			_mouseCursor.x, _mouseCursor.y, _lastRaytraceX, _lastRaytraceY);

	core_trace_scoped(EditorSceneOnProcessUpdateRay);
	_lastRaytraceX = _mouseCursor.x;
	_lastRaytraceY = _mouseCursor.y;

	_result.didHit = false;
	_result.validPreviousPosition = false;
	_result.firstInvalidPosition = false;
	_result.firstValidPosition = false;
	_result.direction = ray.direction;
	_result.hitFace = voxel::FaceNames::Max;

	const math::Axis lockedAxis = _modifierFacade.lockedAxis();
	// TODO: we could optionally limit the raycast to the selection

	voxelutil::raycastWithDirection(v, ray.origin, dirWithLength, [&] (voxel::RawVolume::Sampler& sampler) {
		if (!_result.firstValidPosition && sampler.currentPositionValid()) {
			_result.firstPosition = sampler.position();
			_result.firstValidPosition = true;
		}

		if (sampler.voxel() != air) {
			_result.didHit = true;
			_result.hitVoxel = sampler.position();
			_result.hitFace = voxel::raycastFaceDetection(ray.origin, ray.direction, _result.hitVoxel, 0.0f, 1.0f);
			Log::debug("Raycast face hit: %i", (int)_result.hitFace);
			return false;
		}
		if (sampler.currentPositionValid()) {
			// while having an axis locked, we should end the trace if we hit the plane
			if (lockedAxis != math::Axis::None) {
				const glm::ivec3& cursorPos = cursorPosition();
				if ((lockedAxis & math::Axis::X) != math::Axis::None) {
					if (sampler.position()[0] == cursorPos[0]) {
						return false;
					}
				}
				if ((lockedAxis & math::Axis::Y) != math::Axis::None) {
					if (sampler.position()[1] == cursorPos[1]) {
						return false;
					}
				}
				if ((lockedAxis & math::Axis::Z) != math::Axis::None) {
					if (sampler.position()[2] == cursorPos[2]) {
						return false;
					}
				}
			}

			_result.validPreviousPosition = true;
			_result.previousPosition = sampler.position();
		} else if (_result.firstValidPosition && !_result.firstInvalidPosition) {
			_result.firstInvalidPosition = true;
			_result.hitVoxel = sampler.position();
			return false;
		}
		return true;
	});

	if (_result.firstInvalidPosition) {
		_result.hitFace = voxel::raycastFaceDetection(ray.origin, ray.direction, _result.hitVoxel, 0.0f, 1.0f);
		Log::debug("Raycast face hit: %i", (int)_result.hitFace);
	}

	updateCursor();

	return true;
}

bool SceneManager::nodeUpdatePivot(scenegraph::SceneGraphNode &node, const glm::vec3 &pivot) {
	const glm::vec3 oldPivot = node.pivot();
	if (node.setPivot(pivot)) {
		const glm::vec3 deltaPivot = pivot - oldPivot;
		const glm::vec3 size = node.region().getDimensionsInVoxels();
		const glm::vec3 t = deltaPivot * size;
		Log::debug("oldPivot: %f:%f:%f", oldPivot.x, oldPivot.y, oldPivot.z);
		Log::debug("pivot: %f:%f:%f", pivot.x, pivot.y, pivot.z);
		Log::debug("deltaPivot: %f:%f:%f", deltaPivot.x, deltaPivot.y, deltaPivot.z);
		Log::debug("size: %f:%f:%f", size.x, size.y, size.z);
		Log::debug("t: %f:%f:%f", t.x, t.y, t.z);
		node.translate(-t);
		sceneGraph().updateTransforms();
		markDirty();
		_mementoHandler.markKeyFramesChange(node);
	}
	return true;
}

bool SceneManager::nodeUpdatePivot(int nodeId, const glm::vec3 &pivot) {
	if (nodeId == InvalidNodeId) {
		return false;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeUpdatePivot(*node, pivot);
	}
	return false;
}

bool SceneManager::nodeUpdateKeyFrameInterpolation(scenegraph::SceneGraphNode &node,
												   scenegraph::KeyFrameIndex keyFrameIdx,
												   scenegraph::InterpolationType interpolation) {
	// TODO: check that keyframe already exists
	node.keyFrame(keyFrameIdx).interpolation = interpolation;
	_mementoHandler.markKeyFramesChange(node);
	markDirty();
	return true;
}

bool SceneManager::nodeUpdateKeyFrameInterpolation(int nodeId, scenegraph::KeyFrameIndex keyFrameIdx, scenegraph::InterpolationType interpolation) {
	if (nodeId == InvalidNodeId) {
		return false;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeUpdateKeyFrameInterpolation(*node, keyFrameIdx, interpolation);
	}
	return false;
}

bool SceneManager::nodeTransformMirror(scenegraph::SceneGraphNode &node, scenegraph::KeyFrameIndex keyFrameIdx,
									   math::Axis axis) {
	scenegraph::SceneGraphKeyFrame &keyFrame = node.keyFrame(keyFrameIdx);
	scenegraph::SceneGraphTransform &transform = keyFrame.transform();
	if (axis == math::Axis::X) {
		transform.mirrorX();
	} else if ((axis & (math::Axis::X | math::Axis::Z)) == (math::Axis::X | math::Axis::Z)) {
		transform.mirrorXZ();
	} else if ((axis & (math::Axis::X | math::Axis::Y | math::Axis::Z)) ==
			   (math::Axis::X | math::Axis::Y | math::Axis::Z)) {
		transform.mirrorXYZ();
	} else {
		return false;
	}
	transform.update(_sceneGraph, node, keyFrame.frameIdx, _transformUpdateChildren->boolVal());
	_mementoHandler.markNodeTransform(node, keyFrameIdx);
	markDirty();
	return true;
}

bool SceneManager::nodeTransformMirror(int nodeId, scenegraph::KeyFrameIndex keyFrameIdx, math::Axis axis) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeTransformMirror(*node, keyFrameIdx, axis);
	}
	return false;
}

bool SceneManager::nodeUpdateTransform(int nodeId, const glm::mat4 &matrix,
									   scenegraph::KeyFrameIndex keyFrameIdx, bool local) {
	if (nodeId == InvalidNodeId) {
		nodeForeachGroup([&] (int nodeId) {
			if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
				nodeUpdateTransform(*node, matrix, keyFrameIdx, local);
			}
		});
		return true;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeUpdateTransform(*node, matrix, keyFrameIdx, local);
	}
	return false;
}

bool SceneManager::nodeAddKeyframe(scenegraph::SceneGraphNode &node, scenegraph::FrameIndex frameIdx) {
	const scenegraph::KeyFrameIndex newKeyFrameIdx = node.addKeyFrame(frameIdx);
	if (newKeyFrameIdx == InvalidKeyFrame) {
		Log::warn("Failed to add keyframe for frame %i", (int)frameIdx);
		return false;
	}
	Log::debug("node has %i keyframes", (int)node.keyFrames()->size());
	for (const auto& kf : *node.keyFrames()) {
		Log::debug("- keyframe %i", (int)kf.frameIdx);
	}
	if (newKeyFrameIdx > 0) {
		scenegraph::KeyFrameIndex copyFromKeyFrameIdx = node.previousKeyFrameForFrame(frameIdx);
		core_assert(copyFromKeyFrameIdx != newKeyFrameIdx);
		scenegraph::SceneGraphTransform copyFromTransform = node.keyFrame(copyFromKeyFrameIdx).transform();
		Log::debug("Assign transform from key frame %d to frame %d", (int)copyFromKeyFrameIdx, (int)frameIdx);
		scenegraph::SceneGraphKeyFrame &copyToKeyFrame = node.keyFrame(newKeyFrameIdx);
		copyToKeyFrame.setTransform(copyFromTransform);
		core_assert_msg(copyToKeyFrame.frameIdx == frameIdx, "Expected frame idx %d, got %d", (int)frameIdx, (int)copyToKeyFrame.frameIdx);
		_mementoHandler.markKeyFramesChange(node);
		markDirty();
		return true;
	}
	return false;
}

bool SceneManager::nodeAddKeyFrame(int nodeId, scenegraph::FrameIndex frameIdx) {
	if (nodeId == InvalidNodeId) {
		nodeForeachGroup([&](int nodeId) {
			scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
			nodeAddKeyframe(node, frameIdx);
		});
		return true;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeAddKeyframe(*node, frameIdx);
	}
	return false;
}

bool SceneManager::nodeAllAddKeyFrames(scenegraph::FrameIndex frameIdx) {
	for (auto iter = sceneGraph().beginAllModels(); iter != sceneGraph().end(); ++iter) {
		scenegraph::SceneGraphNode &node = *iter;
		if (!node.hasKeyFrame(frameIdx)) {
			nodeAddKeyframe(node, frameIdx);
		}
	}
	return true;
}

bool SceneManager::nodeRemoveKeyFrame(int nodeId, scenegraph::FrameIndex frameIdx) {
	if (nodeId == InvalidNodeId) {
		nodeForeachGroup([&](int nodeId) {
			scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
			nodeRemoveKeyFrame(node, frameIdx);
		});
		return true;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeRemoveKeyFrame(*node, frameIdx);
	}
	return false;
}

bool SceneManager::nodeRemoveKeyFrameByIndex(int nodeId, scenegraph::KeyFrameIndex keyFrameIdx) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeRemoveKeyFrameByIndex(*node, keyFrameIdx);
	}
	return false;
}

bool SceneManager::nodeRemoveKeyFrame(scenegraph::SceneGraphNode &node, scenegraph::FrameIndex frameIdx) {
	if (node.removeKeyFrame(frameIdx)) {
		_mementoHandler.markKeyFramesChange(node);
		markDirty();
		return true;
	}
	return false;
}

bool SceneManager::nodeRemoveKeyFrameByIndex(scenegraph::SceneGraphNode &node, scenegraph::KeyFrameIndex keyFrameIdx) {
	if (node.removeKeyFrameByIndex(keyFrameIdx)) {
		_mementoHandler.markKeyFramesChange(node);
		markDirty();
		return true;
	}
	return false;
}

bool SceneManager::nodeShiftAllKeyframes(scenegraph::SceneGraphNode &node, const glm::vec3 &shift) {
	if (node.keyFrames() == nullptr) {
		return false;
	}
	for (auto &kf : *node.keyFrames()) {
		scenegraph::SceneGraphTransform &transform = kf.transform();
		const glm::vec3 &newLocalTranslation = transform.localTranslation() + shift;
		transform.setLocalTranslation(newLocalTranslation);
		transform.update(_sceneGraph, node, kf.frameIdx, false);
	}
	_mementoHandler.markKeyFramesChange(node);
	markDirty();
	return true;
}

bool SceneManager::nodeShiftAllKeyframes(int nodeId, const glm::vec3 &shift) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeShiftAllKeyframes(*node, shift);
	}
	return false;
}

bool SceneManager::nodeUpdateTransform(scenegraph::SceneGraphNode &node, const glm::mat4 &matrix,
									   scenegraph::KeyFrameIndex keyFrameIdx, bool local) {
	scenegraph::SceneGraphKeyFrame &keyFrame = node.keyFrame(keyFrameIdx);
	scenegraph::SceneGraphTransform &transform = keyFrame.transform();
	if (local) {
		transform.setLocalMatrix(matrix);
	} else {
		transform.setWorldMatrix(matrix);
	}
	transform.update(_sceneGraph, node, keyFrame.frameIdx, _transformUpdateChildren->boolVal());

	_mementoHandler.markNodeTransform(node, keyFrameIdx);
	markDirty();

	return true;
}

int SceneManager::nodeReference(int nodeId) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		if (node->isReference()) {
			return nodeReference(node->reference());
		}
		return nodeReference(*node);
	}
	return InvalidNodeId;
}

bool SceneManager::nodeDuplicate(int nodeId, int *newNodeId) {
	if (nodeId == InvalidNodeId) {
		return false;
	}
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		nodeDuplicate(*node, newNodeId);
		return true;
	}
	return false;
}

bool SceneManager::nodeMove(int sourceNodeId, int targetNodeId) {
	if (_sceneGraph.changeParent(sourceNodeId, targetNodeId)) {
		core_assert(sceneGraphNode(sourceNodeId) != nullptr);
		_mementoHandler.markNodeMoved(targetNodeId, sourceNodeId);
		markDirty();
		return true;
	}
	return false;
}

bool SceneManager::nodeSetProperty(int nodeId, const core::String &key, const core::String &value) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		if (node->setProperty(key, value)) {
			_mementoHandler.markNodePropertyChange(*node);
			return true;
		}
	}
	return false;
}

bool SceneManager::nodeRemoveProperty(int nodeId, const core::String &key) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		if (node->properties().remove(key)) {
			_mementoHandler.markNodePropertyChange(*node);
			return true;
		}
	}
	return false;
}

bool SceneManager::nodeRename(int nodeId, const core::String &name) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeRename(*node, name);
	}
	return false;
}

bool SceneManager::nodeRename(scenegraph::SceneGraphNode &node, const core::String &name) {
	if (node.name() == name) {
		return true;
	}
	node.setName(name);
	_mementoHandler.markNodeRenamed(node);
	markDirty();
	return true;
}

bool SceneManager::nodeSetVisible(int nodeId, bool visible) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		node->setVisible(visible);
		if (node->type() == scenegraph::SceneGraphNodeType::Group) {
			_sceneGraph.visitChildren(nodeId, true, [visible] (scenegraph::SceneGraphNode &node) {
				node.setVisible(visible);
			});
		}
		return true;
	}
	return false;
}

bool SceneManager::nodeSetLocked(int nodeId, bool locked) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		node->setLocked(locked);
		return true;
	}
	return false;
}

bool SceneManager::nodeRemove(int nodeId, bool recursive) {
	if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
		return nodeRemove(*node, recursive);
	}
	return false;
}

void SceneManager::markDirty() {
	_sceneGraph.markMaxFramesDirty();
	_needAutoSave = true;
	_dirty = true;
}

bool SceneManager::nodeRemove(scenegraph::SceneGraphNode &node, bool recursive) {
	const int nodeId = node.id();
	const core::String name = node.name();
	Log::debug("Delete node %i with name %s", nodeId, name.c_str());
	core::Buffer<int> removeReferenceNodes;
	for (auto iter = _sceneGraph.begin(scenegraph::SceneGraphNodeType::ModelReference); iter != _sceneGraph.end(); ++iter) {
		if ((*iter).reference() == nodeId) {
			removeReferenceNodes.push_back((*iter).id());
		}
	}
	for (int nodeId : removeReferenceNodes) {
		nodeRemove(_sceneGraph.node(nodeId), recursive);
	}
	// TODO: memento and recursive... - we only record the one node in the memento state - not the children
	_mementoHandler.markNodeRemoved(node);
	if (!_sceneGraph.removeNode(nodeId, recursive)) {
		Log::error("Failed to remove node with id %i", nodeId);
		_mementoHandler.removeLast();
		return false;
	}
	_sceneRenderer->removeNode(nodeId);
	if (_sceneGraph.empty()) {
		const voxel::Region region(glm::ivec3(0), glm::ivec3(31));
		scenegraph::SceneGraphNode newNode(scenegraph::SceneGraphNodeType::Model);
		newNode.setVolume(new voxel::RawVolume(region), true);
		if (name.empty()) {
			newNode.setName("unnamed");
		} else {
			newNode.setName(name);
		}
		moveNodeToSceneGraph(newNode);
	} else {
		markDirty();
	}
	return true;
}

void SceneManager::nodeDuplicate(const scenegraph::SceneGraphNode &node, int *newNodeId) {
	const int nodeId = scenegraph::copyNodeToSceneGraph(_sceneGraph, node, node.parent(), true);
	onNewNodeAdded(nodeId, false);
	if (newNodeId) {
		*newNodeId = nodeId;
	}
}

int SceneManager::nodeReference(const scenegraph::SceneGraphNode &node) {
	const int newNodeId = scenegraph::createNodeReference(_sceneGraph, node);
	onNewNodeAdded(newNodeId, false);
	return newNodeId;
}

bool SceneManager::isValidReferenceNode(const scenegraph::SceneGraphNode &node) const {
	if (node.type() != scenegraph::SceneGraphNodeType::ModelReference) {
		Log::error("Node %i is not a reference model", node.id());
		return false;
	}
	if (!_sceneGraph.hasNode(node.reference())) {
		Log::error("Node %i is not valid anymore - referenced node doesn't exist", node.id());
		return false;
	}
	return true;
}

bool SceneManager::nodeUnreference(scenegraph::SceneGraphNode &node) {
	if (!isValidReferenceNode(node)) {
		return false;
	}
	if (scenegraph::SceneGraphNode* referencedNode = sceneGraphNode(node.reference())) {
		if (referencedNode->type() != scenegraph::SceneGraphNodeType::Model) {
			Log::error("Referenced node is no model node - failed to unreference");
			return false;
		}
		if (!node.unreferenceModelNode(*referencedNode)) {
			return false;
		}
		modified(node.id(), node.volume()->region());
		return true;
	}
	Log::error("Referenced node is wasn't found - failed to unreference");
	return false;
}

bool SceneManager::nodeUnreference(int nodeId) {
	if (scenegraph::SceneGraphNode* node = sceneGraphNode(nodeId)) {
		return nodeUnreference(*node);
	}
	return false;
}

bool SceneManager::nodeRemoveColor(scenegraph::SceneGraphNode &node, uint8_t palIdx) {
	voxel::RawVolume *v = _sceneGraph.resolveVolume(node);
	if (v == nullptr) {
		return false;
	}
	palette::Palette &palette = node.palette();
	const uint8_t replacement = palette.findReplacement(palIdx);
	if (replacement != palIdx && palette.removeColor(palIdx)) {
		palette.markSave();
		const voxel::Voxel replacementVoxel = voxel::createVoxel(palette, replacement);
		_mementoHandler.markPaletteChange(node);
		voxelutil::visitVolume(
			*v,
			[v, replacementVoxel](int x, int y, int z, const voxel::Voxel &voxel) {
				v->setVoxel(x, y, z, replacementVoxel);
				return true;
			},
			voxelutil::VisitColor(palIdx));
		return true;
	}
	return false;
}

bool SceneManager::nodeRemoveColor(int nodeId, uint8_t palIdx) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeRemoveColor(*node, palIdx);
	}
	return false;
}

bool SceneManager::nodeDuplicateColor(scenegraph::SceneGraphNode &node, uint8_t palIdx) {
	palette::Palette &palette = node.palette();
	palette.duplicateColor(palIdx);
	palette.markSave();
	_mementoHandler.markPaletteChange(node);
	return true;
}

bool SceneManager::nodeDuplicateColor(int nodeId, uint8_t palIdx) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeDuplicateColor(*node, palIdx);
	}
	return false;
}

bool SceneManager::nodeRemoveAlpha(scenegraph::SceneGraphNode &node, uint8_t palIdx) {
	palette::Palette &palette = node.palette();
	core::RGBA c = palette.color(palIdx);
	if (c.a == 255) {
		return false;
	}
	c.a = 255;
	palette.setColor(palIdx, c);
	palette.markSave();
	_mementoHandler.markPaletteChange(node);
	nodeUpdateVoxelType(node.id(), palIdx, voxel::VoxelType::Generic);
	return true;
}

bool SceneManager::nodeRemoveAlpha(int nodeId, uint8_t palIdx) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeRemoveAlpha(*node, palIdx);
	}
	return false;
}

bool SceneManager::nodeSetMaterial(scenegraph::SceneGraphNode &node, uint8_t palIdx, palette::MaterialProperty material, float value) {
	palette::Palette &palette = node.palette();
	palette.setMaterialValue(palIdx, material, value);
	palette.markSave();
	_mementoHandler.markPaletteChange(node);
	return true;
}

bool SceneManager::nodeSetMaterial(int nodeId, uint8_t palIdx, palette::MaterialProperty material, float value) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeSetMaterial(*node, palIdx, material, value);
	}
	return false;
}

bool SceneManager::nodeSetColor(scenegraph::SceneGraphNode &node, uint8_t palIdx, const core::RGBA &color) {
	palette::Palette &palette = node.palette();
	const bool existingColor = palIdx < palette.colorCount();
	const bool oldHasAlpha = palette.color(palIdx).a != 255;
	palette.setColor(palIdx, color);
	const bool newHasAlpha = color.a != 255;
	if (existingColor) {
		if (oldHasAlpha && !newHasAlpha) {
			nodeUpdateVoxelType(node.id(), palIdx, voxel::VoxelType::Generic);
		} else if (!oldHasAlpha && newHasAlpha) {
			nodeUpdateVoxelType(node.id(), palIdx, voxel::VoxelType::Transparent);
		}
	}
	palette.markSave();
	_mementoHandler.markPaletteChange(node);
	return true;
}

bool SceneManager::nodeSetColor(int nodeId, uint8_t palIdx, const core::RGBA &color) {
	if (scenegraph::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		return nodeSetColor(*node, palIdx, color);
	}
	return false;
}

void SceneManager::nodeForeachGroup(const std::function<void(int)>& f) {
	_sceneGraph.foreachGroup(f);
}

bool SceneManager::nodeActivate(int nodeId) {
	if (nodeId == InvalidNodeId) {
		return false;
	}
	if (!_sceneGraph.hasNode(nodeId)) {
		Log::warn("Given node id %i doesn't exist", nodeId);
		return false;
	}
	if (_sceneGraph.activeNode() == nodeId) {
		return true;
	}
	Log::debug("Activate node %i", nodeId);
	scenegraph::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.type() == scenegraph::SceneGraphNodeType::Camera) {
		video::Camera *camera = activeCamera();
		if (camera == nullptr) {
			return false;
		}
		const scenegraph::SceneGraphNodeCamera& cameraNode = scenegraph::toCameraNode(node);
		video::Camera nodeCamera = voxelrender::toCamera(camera->size(), cameraNode);
		camera->lerp(nodeCamera);
	}
	// a node switch will disable the locked axis as the positions might have changed anyway
	modifier().setLockedAxis(math::Axis::X | math::Axis::Y | math::Axis::Z, true);
	_sceneGraph.setActiveNode(nodeId);
	const palette::Palette &palette = node.palette();
	for (int i = 0; i < palette.colorCount(); ++i) {
		if (palette.color(i).a > 0) {
			_modifierFacade.setCursorVoxel(voxel::createVoxel(palette, i));
			break;
		}
	}
	const voxel::Region& region = node.region();
	_dirtyRenderer |= DirtyRendererGridRenderer;
	if (!region.containsPoint(referencePosition())) {
		const glm::ivec3 pivot = region.getLowerCorner() + glm::ivec3(node.pivot() * glm::vec3(region.getDimensionsInVoxels()));
		setReferencePosition(glm::ivec3(pivot));
	}
	if (!region.containsPoint(cursorPosition())) {
		setCursorPosition(node.region().getCenter());
	}
	resetLastTrace();
	return true;
}

bool SceneManager::empty() const {
	return _sceneGraph.empty();
}

bool SceneManager::cameraRotate() const {
	return _rotate.pressed();
}

bool SceneManager::cameraPan() const {
	return _pan.pressed();
}

void SceneManager::setActiveCamera(video::Camera *camera) {
	if (_camera == camera) {
		return;
	}
	_camera = camera;
	resetLastTrace();
}

}
