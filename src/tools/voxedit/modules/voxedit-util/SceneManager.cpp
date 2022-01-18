/**
 * @file
 */

#include "SceneManager.h"

#include "animation/Animation.h"
#include "core/collection/DynamicArray.h"
#include "io/FileStream.h"
#include "math/AABB.h"
#include "voxedit-ui/LayerPanel.h"
#include "voxel/RawVolume.h"
#include "voxelformat/SceneGraph.h"
#include "voxelformat/SceneGraphNode.h"
#include "voxelutil/VolumeMerger.h"
#include "voxelutil/VolumeCropper.h"
#include "voxelutil/VolumeRotator.h"
#include "voxelutil/VolumeMover.h"
#include "voxelutil/VolumeRescaler.h"
#include "voxelutil/VolumeVisitor.h"
#include "voxel/RawVolumeWrapper.h"
#include "voxel/RawVolumeMoveWrapper.h"
#include "voxelutil/Picking.h"
#include "voxel/Face.h"
#include "voxelgenerator/TreeGenerator.h"
#include "voxelworld/BiomeManager.h"
#include "voxelformat/VolumeFormat.h"
#include "voxelformat/VoxFormat.h"
#include "voxelformat/QBTFormat.h"
#include "voxelformat/CubFormat.h"
#include "voxelformat/QBFormat.h"
#include "voxelformat/VXMFormat.h"
#include "video/ScopedPolygonMode.h"
#include "video/ScopedLineWidth.h"
#include "video/ScopedBlendMode.h"
#include "math/Ray.h"
#include "math/Random.h"
#include "math/Axis.h"
#include "command/Command.h"
#include "command/CommandCompleter.h"
#include "core/ArrayLength.h"
#include "app/App.h"
#include "core/Log.h"
#include "core/Color.h"
#include "core/StringUtil.h"
#include "core/GLM.h"
#include "io/Filesystem.h"
#include "render/Gizmo.h"

#include "AxisUtil.h"
#include "CustomBindingContext.h"
#include "Config.h"
#include "tool/Clipboard.h"
#include "tool/Resize.h"
#include "voxelutil/ImageUtils.h"
#include "anim/AnimationLuaSaver.h"
#include "core/TimeProvider.h"
#include "attrib/ShadowAttributes.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/transform.hpp>

namespace voxedit {

SceneManager::~SceneManager() {
	core_assert_msg(_initialized == 0, "SceneManager was not properly shut down");
}

bool SceneManager::loadPalette(const core::String& paletteName) {
	const io::FilesystemPtr& filesystem = io::filesystem();
	const io::FilePtr& paletteFile = filesystem->open(core::string::format("palette-%s.png", paletteName.c_str()));
	const io::FilePtr& luaFile = filesystem->open(core::string::format("palette-%s.lua", paletteName.c_str()));
	if (voxel::overrideMaterialColors(paletteFile, luaFile)) {
		core::Var::getSafe(cfg::VoxEditLastPalette)->setVal(paletteName);
		return true;
	}
	return false;
}

bool SceneManager::importPalette(const core::String& file) {
	const core::String luaString = "";
	const core::String& ext = core::string::extractExtension(file);
	const core::String paletteName(core::string::extractFilename(file.c_str()));
	const core::String& paletteFilename = core::string::format("palette-%s.png", paletteName.c_str());

	core::Array<uint32_t, 256> buf;
	bool paletteLoaded = false;
	for (const io::FormatDescription* desc = io::format::images(); desc->name != nullptr; ++desc) {
		if (ext == desc->ext) {
			const image::ImagePtr &img = image::loadImage(file, false);
			if (!img->isLoaded()) {
				Log::warn("Failed to load image %s", file.c_str());
				break;
			}
			if (!voxel::createPalette(img, buf.begin(), buf.size())) {
				Log::warn("Failed to create palette for image %s", file.c_str());
				return false;
			}
			if (!voxel::overrideMaterialColors((const uint8_t*)buf.begin(), buf.size() * sizeof(uint32_t), luaString)) {
				Log::warn("Failed to import palette for image %s", file.c_str());
				return false;
			}
			paletteLoaded = true;
			break;
		}
	}
	const io::FilesystemPtr& fs = io::filesystem();
	if (!paletteLoaded) {
		const io::FilePtr& palFile = fs->open(file);
		if (!palFile->validHandle()) {
			Log::warn("Failed to load palette from %s", file.c_str());
			return false;
		}
		io::FileStream stream(palFile);
		if (voxelformat::loadPalette(file, stream, buf) <= 0) {
			Log::warn("Failed to load palette from %s", file.c_str());
			return false;
		}
		if (!voxel::overrideMaterialColors((const uint8_t*)buf.begin(), buf.size() * sizeof(uint32_t), luaString)) {
			Log::warn("Failed to import palette for model %s", file.c_str());
			return false;
		}
	}
	const io::FilePtr& pngFile = fs->open(paletteFilename, io::FileMode::Write);
	if (image::Image::writePng(pngFile->name().c_str(), (const uint8_t*)buf.begin(), buf.size(), 1, 4)) {
		fs->write(core::string::format("palette-%s.lua", paletteName.c_str()), luaString);
		core::Var::getSafe(cfg::VoxEditLastPalette)->setVal(paletteName);
	} else {
		Log::warn("Failed to write image");
	}
	return true;
}

bool SceneManager::importAsPlane(const core::String& file) {
	const image::ImagePtr& img = image::loadImage(file, false);
	voxel::SceneGraphNode node;
	node.setVolume(voxelutil::importAsPlane(img), true);
	node.setName(core::string::extractFilename(img->name().c_str()));
	return addNodeToSceneGraph(node) != -1;
}

// TODO: should create a new model node - see importAsPlane
bool SceneManager::importHeightmap(const core::String& file) {
	const int nodeId = activeNode();
	voxel::RawVolume* v = volume(nodeId);
	if (v == nullptr) {
		return false;
	}
	const image::ImagePtr& img = image::loadImage(file, false);
	if (!img->isLoaded()) {
		return false;
	}
	voxel::RawVolumeWrapper wrapper(v);
	voxelutil::importHeightmap(wrapper, img);
	modified(nodeId, wrapper.dirtyRegion());
	return true;
}

void SceneManager::autosave() {
	if (!_needAutoSave) {
		return;
	}
	const core::TimeProviderPtr& timeProvider = app::App::getInstance()->timeProvider();
	const double delay = (double)_autoSaveSecondsDelay->intVal();
	if (_lastAutoSave + delay > timeProvider->tickSeconds()) {
		return;
	}
	core::String autoSaveFilename;
	if (_lastFilename.empty()) {
		autoSaveFilename = "autosave-noname.vox";
	} else {
		if (core::string::startsWith(_lastFilename.c_str(), "autosave-")) {
			autoSaveFilename = _lastFilename;
		} else {
			const io::FilePtr file = io::filesystem()->open(_lastFilename);
			const core::String& p = file->path();
			const core::String& f = file->fileName();
			const core::String& e = file->extension();
			autoSaveFilename = core::string::format("%sautosave-%s.%s",
					p.c_str(), f.c_str(), e.c_str());
		}
	}
	if (save(autoSaveFilename, true)) {
		Log::info("Autosave file %s", autoSaveFilename.c_str());
	} else {
		Log::warn("Failed to autosave");
	}
	_lastAutoSave = timeProvider->tickSeconds();
}

bool SceneManager::saveNode(int nodeId, const core::String& file) {
	const io::FilePtr& filePtr = io::filesystem()->open(file, io::FileMode::SysWrite);
	if (!filePtr->validHandle()) {
		Log::warn("Failed to open the given file '%s' for writing", file.c_str());
		return false;
	}
	const voxel::SceneGraphNode *node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return true;
	}
	voxel::SceneGraph newSceneGraph;
	voxel::SceneGraphNode newNode;
	// TODO: also add all children
	newNode.setVolume(node->volume(), false);
	newNode.setName(node->name());
	newNode.setVisible(node->visible());
	newSceneGraph.emplace(core::move(newNode));
	if (voxelformat::saveFormat(filePtr, newSceneGraph)) {
		Log::info("Saved layer %i to %s", nodeId, filePtr->name().c_str());
		return true;
	}
	Log::warn("Failed to save layer %i to %s", nodeId, filePtr->name().c_str());
	return false;
}

bool SceneManager::saveModels(const core::String& dir) {
	bool state = false;
	for (const voxel::SceneGraphNode & node : _sceneGraph) {
		const core::String filename = dir + "/" + node.name() + ".qb";
		state |= saveNode(node.id(), filename);
	}
	return state;
}

bool SceneManager::save(const core::String& file, bool autosave) {
	if (_sceneGraph.empty()) {
		Log::warn("No volumes for saving found");
		return false;
	}

	if (file.empty()) {
		Log::warn("No filename given for saving");
		return false;
	}
	const io::FilePtr& filePtr = io::filesystem()->open(file, io::FileMode::SysWrite);
	if (!filePtr->validHandle()) {
		Log::warn("Failed to open the given file '%s' for writing", file.c_str());
		return false;
	}
	core::String ext = filePtr->extension();
	if (ext.empty()) {
		Log::warn("No file extension given for saving, assuming qb");
		ext = "qb";
	}

	bool saved = voxelformat::saveFormat(filePtr, _sceneGraph);
	if (!saved) {
		Log::warn("Failed to save %s file - retry as qb instead", ext.c_str());
		voxel::QBFormat f;
		io::FileStream stream(filePtr.get());
		saved = f.saveGroups(_sceneGraph, filePtr->fileName(), stream);
	}
	if (saved) {
		if (!autosave) {
			_dirty = false;
			_lastFilename = file;
		}
		core::Var::get(cfg::VoxEditLastFile)->setVal(file);
		_needAutoSave = false;
	} else {
		Log::warn("Failed to save to desired format");
	}
	return saved;
}

bool SceneManager::prefab(const core::String& file) {
	if (file.empty()) {
		return false;
	}
	const io::FilePtr& filePtr = io::filesystem()->open(file);
	if (!filePtr->validHandle()) {
		Log::error("Failed to open model file %s", file.c_str());
		return false;
	}
	voxel::SceneGraph newSceneGraph;
	io::FileStream stream(filePtr);
	voxelformat::loadFormat(filePtr->name(), stream, newSceneGraph);
	return addSceneGraphNodes(newSceneGraph) > 0;
}

bool SceneManager::load(const core::String& file) {
	if (file.empty()) {
		return false;
	}
	const io::FilePtr& filePtr = io::filesystem()->open(file);
	if (!filePtr->validHandle()) {
		Log::error("Failed to open model file '%s'", file.c_str());
		return false;
	}
	voxel::SceneGraph newSceneGraph;
	io::FileStream stream(filePtr);
	voxelformat::loadFormat(filePtr->name(), stream, newSceneGraph);
	if (!loadSceneGraph(newSceneGraph)) {
		return false;
	}
	_lastFilename = filePtr->fileName() + "." + filePtr->extension();
	_needAutoSave = false;
	_dirty = false;
	return true;
}

void SceneManager::setMousePos(int x, int y) {
	if (_mouseCursor.x == x && _mouseCursor.y == y) {
		return;
	}
	_mouseCursor.x = x;
	_mouseCursor.y = y;
	_traceViaMouse = true;
}

void SceneManager::handleAnimationViewUpdate(int nodeId) {
	if (!_animationUpdate && _animationNodeIdDirtyState == -1) {
		// the first layer
		_animationNodeIdDirtyState = nodeId;
	} else if (_animationUpdate) {
		// a second layer was modified (maybe a group action)
		_animationNodeIdDirtyState = -1;
	}
	_animationUpdate = true;
}

void SceneManager::queueRegionExtraction(int nodeId, const voxel::Region& region) {
	bool addNew = true;
	for (const auto& r : _extractRegions) {
		if (r.nodeId != nodeId) {
			continue;
		}
		if (r.region.containsRegion(region)) {
			addNew = false;
			break;
		}
	}
	if (addNew) {
		_extractRegions.push_back({region, nodeId});
	}
}

void SceneManager::modified(int nodeId, const voxel::Region& modifiedRegion, bool markUndo) {
	Log::debug("Modified node %i, undo state: %s", nodeId, markUndo ? "true" : "false");
	voxel::logRegion("Modified", modifiedRegion);
	if (markUndo) {
		voxel::SceneGraphNode &node = _sceneGraph.node(nodeId);
		_mementoHandler.markUndo(node.parent(), nodeId, node.name(), node.volume(), MementoType::Modification, modifiedRegion);
	}
	if (modifiedRegion.isValid()) {
		queueRegionExtraction(nodeId, modifiedRegion);
	}
	_dirty = true;
	_needAutoSave = true;
	handleAnimationViewUpdate(nodeId);
	resetLastTrace();
}

void SceneManager::colorToNewLayer(const voxel::Voxel voxelColor) {
	voxel::RawVolume* newVolume = new voxel::RawVolume(_sceneGraph.region());
	_sceneGraph.foreachGroup([&] (int nodeId) {
		voxel::RawVolumeWrapper wrapper(volume(nodeId));
		voxelutil::visitVolume(wrapper, [&] (int32_t x, int32_t y, int32_t z, const voxel::Voxel& voxel) {
			if (voxel.getColor() == voxelColor.getColor()) {
				newVolume->setVoxel(x, y, z, voxel);
				wrapper.setVoxel(x, y, z, voxel::Voxel());
			}
		});
		modified(nodeId, wrapper.dirtyRegion());
	});
	voxel::SceneGraphNode node;
	node.setVolume(newVolume, true);
	node.setName(core::string::format("color: %i", (int)voxelColor.getColor()));
	addNodeToSceneGraph(node);
}

void SceneManager::scale(int nodeId) {
	voxel::RawVolume* srcVolume = volume(nodeId);
	if (srcVolume == nullptr) {
		return;
	}
	const voxel::Region srcRegion = srcVolume->region();
	const glm::ivec3& targetDimensionsHalf = (srcRegion.getDimensionsInVoxels() / 2) - 1;
	if (targetDimensionsHalf.x < 0 || targetDimensionsHalf.y < 0 || targetDimensionsHalf.z < 0) {
		Log::debug("Can't scale anymore");
		return;
	}
	const voxel::Region destRegion(srcRegion.getLowerCorner(), srcRegion.getLowerCorner() + targetDimensionsHalf);
	voxel::RawVolume* destVolume = new voxel::RawVolume(destRegion);
	rescaleVolume(*srcVolume, *destVolume);
	if (!setNewVolume(nodeId, destVolume, true)) {
		delete destVolume;
		return;
	}
	modified(nodeId, srcRegion);
}

void SceneManager::crop() {
	const int nodeId = activeNode();
	voxel::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return;
	}
	if (_volumeRenderer.empty(*node)) {
		Log::info("Empty volumes can't be cropped");
		return;
	}
	voxel::RawVolume* newVolume = voxel::cropVolume(node->volume());
	if (newVolume == nullptr) {
		return;
	}
	if (!setNewVolume(nodeId, newVolume, true)) {
		delete newVolume;
		return;
	}
	modified(nodeId, newVolume->region());
}

void SceneManager::resize(int nodeId, const glm::ivec3& size) {
	voxel::RawVolume* newVolume = voxedit::tool::resize(volume(nodeId), size);
	if (newVolume == nullptr) {
		return;
	}
	if (!setNewVolume(nodeId, newVolume, false)) {
		delete newVolume;
		return;
	}
	if (glm::all(glm::greaterThanEqual(size, glm::zero<glm::ivec3>()))) {
		// we don't have to reextract a mesh if only new empty voxels were added.
		modified(nodeId, voxel::Region::InvalidRegion);
	} else {
		modified(nodeId, newVolume->region());
	}
}

void SceneManager::resizeAll(const glm::ivec3& size) {
	const glm::ivec3 refPos = referencePosition();
	_sceneGraph.foreachGroup([&] (int nodeId) {
		resize(nodeId, size);
	});
	if (_sceneGraph.region().containsPoint(refPos)) {
		setReferencePosition(refPos);
	}
}

voxel::RawVolume* SceneManager::volume(int nodeId) {
	voxel::SceneGraphNode* node = sceneGraphNode(nodeId);
	core_assert_msg(node != nullptr, "Node with id %i wasn't found in the scene graph", nodeId);
	return node->volume();
}

const voxel::RawVolume* SceneManager::volume(int nodeId) const {
	const voxel::SceneGraphNode* node = sceneGraphNode(nodeId);
	core_assert_msg(node != nullptr, "Node with id %i wasn't found in the scene graph", nodeId);
	return node->volume();
}

int SceneManager::activeNode() const {
	return _sceneGraph.activeNode();
}

voxel::RawVolume* SceneManager::activeVolume() {
	return volume(activeNode());
}

void SceneManager::undo() {
	const MementoState& s = _mementoHandler.undo();
	ScopedMementoHandlerLock lock(_mementoHandler);
	if (s.type == MementoType::LayerRenamed) {
		if (voxel::SceneGraphNode* node = sceneGraphNode(s.nodeId)) {
			node->setName(s.name);
		}
		return;
	}
	voxel::RawVolume* v = MementoData::toVolume(s.data);
	if (v == nullptr) {
		nodeRemove(s.nodeId);
		return;
	}
	Log::debug("Volume found in undo state for layer: %i with name %s", s.nodeId, s.name.c_str());
	voxel::SceneGraphNode node(voxel::SceneGraphNodeType::Model);
	node.setName(s.name);
	node.setVolume(v, true);
	node.setPivot(referencePosition());
	addNodeToSceneGraph(node, s.parentId);
}

void SceneManager::redo() {
	const MementoState& s = _mementoHandler.redo();
	ScopedMementoHandlerLock lock(_mementoHandler);
	if (s.type == MementoType::LayerRenamed) {
		if (voxel::SceneGraphNode* node = sceneGraphNode(s.nodeId)) {
			node->setName(s.name);
		}
		return;
	}
	voxel::RawVolume* v = MementoData::toVolume(s.data);
	if (v == nullptr) {
		nodeRemove(s.nodeId);
		return;
	}
	Log::debug("Volume found in redo state for layer: %i with name %s", s.nodeId, s.name.c_str());
	voxel::SceneGraphNode node(voxel::SceneGraphNodeType::Model);
	node.setName(s.name);
	node.setVolume(v, true);
	node.setPivot(referencePosition());
	addNodeToSceneGraph(node, s.parentId);
}

void SceneManager::copy() {
	const Selection& selection = _modifier.selection();
	if (!selection.isValid()) {
		return;
	}
	const int nodeId = activeNode();
	voxel::RawVolume* model = volume(nodeId);
	if (_copy != nullptr) {
		delete _copy;
	}
	_copy = voxedit::tool::copy(model, selection);
}

void SceneManager::paste(const glm::ivec3& pos) {
	if (_copy == nullptr) {
		Log::debug("Nothing copied yet - failed to paste");
		return;
	}
	const int nodeId = activeNode();
	voxel::RawVolume* model = volume(nodeId);
	voxel::Region modifiedRegion;
	voxedit::tool::paste(model, _copy, pos, modifiedRegion);
	if (!modifiedRegion.isValid()) {
		Log::debug("Failed to paste");
		return;
	}
	modified(nodeId, modifiedRegion);
}

void SceneManager::cut() {
	const Selection& selection = _modifier.selection();
	if (!selection.isValid()) {
		Log::debug("Nothing selected - failed to cut");
		return;
	}
	const int nodeId = activeNode();
	voxel::RawVolume* model = volume(nodeId);
	if (_copy != nullptr) {
		delete _copy;
	}
	voxel::Region modifiedRegion;
	_copy = voxedit::tool::cut(model, selection, modifiedRegion);
	if (_copy == nullptr) {
		Log::debug("Failed to cut");
		return;
	}
	modified(nodeId, modifiedRegion);
}

void SceneManager::resetLastTrace() {
	_sceneModeNodeIdTrace = -1;
	if (!_traceViaMouse) {
		return;
	}
	_lastRaytraceX = _lastRaytraceY = -1;
}

bool SceneManager::mergeMultiple(LayerMergeFlags flags) {
	core::DynamicArray<const voxel::RawVolume*> volumes;
	for (voxel::SceneGraphNode &node : _sceneGraph) {
		const voxel::RawVolume* v = node.volume();
		if ((flags & LayerMergeFlags::All) != LayerMergeFlags::None) {
			volumes.push_back(v);
		} else if ((flags & LayerMergeFlags::Visible) != LayerMergeFlags::None) {
			if (node.visible()) {
				volumes.push_back(v);
			}
		} else if ((flags & LayerMergeFlags::Invisible) != LayerMergeFlags::None) {
			if (!node.visible()) {
				volumes.push_back(v);
			}
		} else if ((flags & LayerMergeFlags::Locked) != LayerMergeFlags::None) {
			if (node.locked()) {
				volumes.push_back(v);
			}
		}
	}

	if (volumes.size() <= 1) {
		return false;
	}

	voxel::RawVolume* merged = voxel::merge(volumes);
	voxel::SceneGraph newSceneGraph;
	voxel::SceneGraphNode node;
	node.setVolume(merged, true);
	newSceneGraph.emplace(core::move(node));
	addSceneGraphNodes(newSceneGraph);
	// TODO: broken - remove old nodes
	return true;
}

bool SceneManager::merge(int nodeId1, int nodeId2) {
	core::DynamicArray<const voxel::RawVolume*> volumes;
	volumes.resize(2);
	volumes[0] = volume(nodeId1);
	if (volumes[0] == nullptr) {
		return false;
	}
	volumes[1] = volume(nodeId2);
	if (volumes[1] == nullptr) {
		return false;
	}
	voxel::RawVolume* volume = voxel::merge(volumes);
	if (!setNewVolume(nodeId1, volume, true)) {
		delete volume;
		return false;
	}
	// TODO: the memento states are not yet perfect
	modified(nodeId1, volume->region(), true);
	nodeRemove(nodeId2);
	return true;
}

void SceneManager::resetSceneState() {
	_animationNodeIdDirtyState = -1;
	_animationIdx = 0;
	_animationUpdate = false;
	_editMode = EditMode::Model;
	_mementoHandler.clearStates();
	voxel::SceneGraphNode *node = _sceneGraph[0];
	core_assert_always(node != nullptr);
	_sceneGraph.setActiveNode(node->id());
	Log::debug("New volume for node %i", node->id());
	_mementoHandler.markUndo(node->parent(), node->id(), node->name(), node->volume());
	_dirty = false;
	_result = voxel::PickResult();
	setCursorPosition(cursorPosition(), true);
	resetLastTrace();
}

int SceneManager::addNodeToSceneGraph(voxel::SceneGraphNode &node, int parent) {
	voxel::SceneGraphNode newNode(node.type());
	newNode.setName(node.name());
	newNode.setPivot(node.pivot());
	newNode.setVisible(node.visible());
	newNode.addProperties(node.properties());
	newNode.setReferencedNodeId(node.referencedNodeId());
	newNode.setMatrix(node.matrix());
	if (newNode.type() == voxel::SceneGraphNodeType::Model) {
		core_assert(node.owns());
		newNode.setVolume(node.volume(), true);
		node.releaseOwnership();
	}

	const int newNodeId = _sceneGraph.emplace(core::move(newNode), parent);
	Log::debug("Add node %i to scene graph", newNodeId);
	if (newNode.type() == voxel::SceneGraphNodeType::Model) {
		// update the whole volume
		const voxel::Region& region = node.region();
		queueRegionExtraction(newNodeId, region);

		Log::debug("Adding node %i with name %s", newNodeId, node.name().c_str());
		_mementoHandler.markNodeAdded(parent, newNodeId, node.name(), node.volume());

		_result = voxel::PickResult();
		_needAutoSave = true;
		_dirty = true;

		nodeActivate(newNodeId);
		handleAnimationViewUpdate(newNodeId);
	}
	return newNodeId;
}

int SceneManager::addSceneGraphNode_r(voxel::SceneGraph &sceneGraph, voxel::SceneGraphNode &node, int parent) {
	const int newNodeId = addNodeToSceneGraph(node, parent);
	if (newNodeId == -1) {
		return 0;
	}
	int modelsAdded = node.type() == voxel::SceneGraphNodeType::Model ? 1 : 0;
	for (int nodeIdx : node.children()) {
		core_assert(sceneGraph.hasNode(nodeIdx));
		voxel::SceneGraphNode &childNode = sceneGraph.node(nodeIdx);
		const voxel::SceneGraphNodeType childType = childNode.type();
		const int childNodeId = addSceneGraphNode_r(sceneGraph, childNode, newNodeId);
		if (childNodeId == -1) {
			Log::error("Failed to load scene graph node %i of type %i", nodeIdx, (int)childType);
			continue;
		}
		if (childType == voxel::SceneGraphNodeType::Model) {
			++modelsAdded;
		}
	}
	return modelsAdded;
}

int SceneManager::addSceneGraphNodes(voxel::SceneGraph& sceneGraph) {
	const voxel::SceneGraphNode &root = sceneGraph.root();
	int modelsAdded = 0;
	for (int nodeId : root.children()) {
		modelsAdded += addSceneGraphNode_r(sceneGraph, sceneGraph.node(nodeId), 0);
	}
	return modelsAdded;
}

bool SceneManager::loadSceneGraph(voxel::SceneGraph& sceneGraph) {
	core_trace_scoped(LoadSceneGraph);
	_sceneGraph.clear();
	_volumeRenderer.clear();

	const int modelsAdded = addSceneGraphNodes(sceneGraph);
	if (modelsAdded == 0) {
		Log::warn("Failed to load any model volumes");
		const voxel::Region region(glm::ivec3(0), glm::ivec3(size() - 1));
		newScene(true, "", region);
		return false;
	}
	resetSceneState();
	return true;
}

static inline math::AABB<float> toAABB(const voxel::Region& region) {
	core_assert(region.isValid());
	const math::AABB<int> intaabb(region.getLowerCorner(), region.getUpperCorner() + 1);
	return math::AABB<float>(glm::vec3(intaabb.getLowerCorner()), glm::vec3(intaabb.getUpperCorner()));
}

void SceneManager::updateGridRenderer(const voxel::Region& region) {
	_gridRenderer.update(toAABB(region));
}

voxel::SceneGraphNode *SceneManager::sceneGraphNode(int nodeId) {
	if (_sceneGraph.hasNode(nodeId)) {
		return &_sceneGraph.node(nodeId);
	}
	return nullptr;
}

const voxel::SceneGraphNode *SceneManager::sceneGraphNode(int nodeId) const {
	if (_sceneGraph.hasNode(nodeId)) {
		return &_sceneGraph.node(nodeId);
	}
	return nullptr;
}

const voxel::SceneGraph &SceneManager::sceneGraph() {
	return _sceneGraph;
}

// TODO: handle deleteMesh somehow
bool SceneManager::setNewVolume(int nodeId, voxel::RawVolume* volume, bool deleteMesh) {
	core_trace_scoped(SetNewVolume);
	voxel::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return false;
	}
	return setSceneGraphNodeVolume(*node, volume);
}

bool SceneManager::setSceneGraphNodeVolume(voxel::SceneGraphNode &node, voxel::RawVolume* volume) {
	node.setVolume(volume, true);

	const voxel::Region& region = volume->region();
	updateGridRenderer(region);
	updateAABBMesh();

	_dirty = false;
	_result = voxel::PickResult();
	setCursorPosition(cursorPosition(), true);
	glm::ivec3 center = region.getCenter();
	center.y = region.getLowerY();
	setReferencePosition(center);
	resetLastTrace();
	return true;
}

bool SceneManager::newScene(bool force, const core::String& name, const voxel::Region& region) {
	if (dirty() && !force) {
		return false;
	}
	_sceneGraph.clear();
	_volumeRenderer.clear();

	voxel::RawVolume* v = new voxel::RawVolume(region);
	voxel::SceneGraphNode node;
	node.setVolume(v, true);
	node.setName(name);
	glm::ivec3 center = region.getCenter();
	node.setPivot(center);
	const int nodeId = sceneMgr().addNodeToSceneGraph(node);
	if (nodeId == -1) {
		Log::error("Failed to add empty volume to new scene graph");
		return false;
	}
	center.y = region.getLowerY();
	setReferencePosition(center);
	resetSceneState();
	return true;
}

void SceneManager::rotate(int nodeId, const glm::ivec3& angle, bool increaseSize, bool rotateAroundReferencePosition) {
	const voxel::RawVolume* model = volume(nodeId);
	if (model == nullptr) {
		return;
	}
	voxel::RawVolume* newVolume;
	const bool axisRotation = !rotateAroundReferencePosition && !increaseSize;
	if (axisRotation && angle == glm::ivec3(90, 0, 0)) {
		newVolume = voxel::rotateAxis(model, math::Axis::X);
	} else if (axisRotation && angle == glm::ivec3(0, 90, 0)) {
		newVolume = voxel::rotateAxis(model, math::Axis::Y);
	} else if (axisRotation && angle == glm::ivec3(0, 0, 90)) {
		newVolume = voxel::rotateAxis(model, math::Axis::Z);
	} else {
		const glm::vec3 pivot = rotateAroundReferencePosition ? referencePosition() : model->region().getCenter();
		newVolume = voxel::rotateVolume(model, angle, pivot, increaseSize);
	}
	voxel::Region r = newVolume->region();
	r.accumulate(model->region());
	if (!setNewVolume(nodeId, newVolume)) {
		delete newVolume;
		return;
	}
	modified(nodeId, r);
}

void SceneManager::rotate(int angleX, int angleY, int angleZ, bool increaseSize, bool rotateAroundReferencePosition) {
	const glm::ivec3 angle(angleX, angleY, angleZ);
	_sceneGraph.foreachGroup([&] (int nodeId) {
		rotate(nodeId, angle, increaseSize, rotateAroundReferencePosition);
	});
}

void SceneManager::move(int nodeId, const glm::ivec3& m) {
	const voxel::RawVolume* model = volume(nodeId);
	voxel::RawVolume* newVolume = new voxel::RawVolume(model->region());
	voxel::RawVolumeMoveWrapper wrapper(newVolume);
	voxel::moveVolume(&wrapper, model, m);
	if (!setNewVolume(nodeId, newVolume)) {
		delete newVolume;
		return;
	}
	modified(nodeId, newVolume->region());
}

void SceneManager::move(int x, int y, int z) {
	const glm::ivec3 v(x, y, z);
	_sceneGraph.foreachGroup([&] (int nodeId) {
		move(nodeId, v);
	});
}

void SceneManager::shift(int nodeId, const glm::ivec3& m) {
	voxel::SceneGraphNode* node = sceneGraphNode(nodeId);
	if (node == nullptr) {
		return;
	}
	voxel::RawVolume* model = node->volume();
	Log::debug("Shift region by %s on layer %i", glm::to_string(m).c_str(), nodeId);
	voxel::Region oldRegion = model->region();
	setReferencePosition(_referencePos + m);
	_modifier.translate(m);
	_volumeRenderer.translate(*node, m);
	setGizmoPosition();
	const voxel::Region& newRegion = model->region();
	updateGridRenderer(newRegion);
	updateAABBMesh();
	oldRegion.accumulate(newRegion);
	modified(nodeId, oldRegion);
}

void SceneManager::shift(int x, int y, int z) {
	const glm::ivec3 v(x, y, z);
	_sceneGraph.foreachGroup([&] (int nodeId) {
		shift(nodeId, v);
	});
}

void SceneManager::executeGizmoAction(const glm::ivec3& delta, render::GizmoMode mode) {
	// TODO: memento state at pressing and releasing
	if (mode == render::GizmoMode::TranslateX) {
		if (delta.x != 0) {
			shift(delta.x, 0, 0);
		}
	} else if (mode == render::GizmoMode::TranslateY) {
		if (delta.y != 0) {
			shift(0, delta.y, 0);
		}
	} else if (mode == render::GizmoMode::TranslateZ) {
		if (delta.z != 0) {
			shift(0, 0, delta.z);
		}
	}
}

bool SceneManager::setGridResolution(int resolution) {
	const bool ret = gridRenderer().setGridResolution(resolution);
	if (!ret) {
		return false;
	}

	const int res = gridRenderer().gridResolution();
	_modifier.setGridResolution(res);
	setCursorPosition(cursorPosition(), true);

	return true;
}

void SceneManager::renderAnimation(const video::Camera& camera) {
	attrib::ShadowAttributes attrib;
	const double deltaFrameSeconds = app::App::getInstance()->deltaFrameSeconds();
	if (_animationUpdate) {
		for (voxel::SceneGraphNode& node : _sceneGraph) {
			if (_animationNodeIdDirtyState >= 0 && _animationNodeIdDirtyState != node.id()) {
				Log::debug("Don't update layer %i", node.id());
				continue;
			}
			const core::String& value = node.property("type");
			if (value.empty()) {
				Log::debug("No type metadata found on layer %i", node.id());
				continue;
			}
			const int characterMeshTypeId = core::string::toInt(value);
			const animation::AnimationSettings& animSettings = animationEntity().animationSettings();
			const core::String& path = animSettings.paths[characterMeshTypeId];
			if (path.empty()) {
				Log::debug("No path found for layer %i", node.id());
				continue;
			}
			voxel::Mesh mesh;
			_volumeRenderer.toMesh(node, &mesh);
			const core::String& fullPath = animSettings.fullPath(characterMeshTypeId);
			_animationCache->putMesh(fullPath.c_str(), mesh);
			Log::debug("Updated mesh on layer %i for path %s", node.id(), fullPath.c_str());
		}
		if (!animationEntity().initMesh(_animationCache)) {
			Log::warn("Failed to update the mesh");
		}
		_animationUpdate = false;
		_animationNodeIdDirtyState = -1;
	}
	animationEntity().update(deltaFrameSeconds, attrib);
	_animationRenderer.render(animationEntity(), camera);
}

void SceneManager::updateAABBMesh() {
	Log::debug("Update aabb mesh");
	_shapeBuilder.clear();
	_shapeBuilder.setColor(core::Color::Gray);
	for (voxel::SceneGraphNode &node : _sceneGraph) {
		const voxel::RawVolume* v = node.volume();
		const voxel::Region& region = v->region();
		_shapeBuilder.aabb(toAABB(region));
	}
	const voxel::RawVolume* mdl = activeVolume();
	const voxel::Region& region = mdl->region();
	_shapeBuilder.setColor(core::Color::White);
	_shapeBuilder.aabb(toAABB(region));
	_shapeRenderer.createOrUpdate(_aabbMeshIndex, _shapeBuilder);
}

void SceneManager::render(const video::Camera& camera, uint8_t renderMask) {
	const bool depthTest = video::enable(video::State::DepthTest);
	const bool renderUI = (renderMask & RenderUI) != 0u;
	const bool renderScene = (renderMask & RenderScene) != 0u;
	if (renderUI) {
		if (_editMode == EditMode::Scene) {
			_shapeRenderer.render(_aabbMeshIndex, camera);
		} else {
			const voxel::RawVolume *v = activeVolume();
			const voxel::Region& region = v->region();
			_gridRenderer.render(camera, toAABB(region));
		}
	}
	if (renderScene) {
		std::function<bool(int)> func;
		if (_grayScale->boolVal()) {
			func = [&](int nodeId) { return nodeId != activeNode(); };
		} else {
			func = [&](int nodeId) { return false; };
		}
		_volumeRenderer.setRenderScene(_editMode == EditMode::Scene);
		_volumeRenderer.render(_sceneGraph, false, camera, _renderShadow, func);
		extractVolume();
	}
	if (renderUI) {
		if (_editMode == EditMode::Scene) {
			_gizmo.render(camera);
		} else {
			_modifier.render(camera);

			// TODO: render error if rendered last - but be before grid renderer to get transparency.
			if (_renderLockAxis) {
				for (int i = 0; i < lengthof(_planeMeshIndex); ++i) {
					_shapeRenderer.render(_planeMeshIndex[i], camera);
				}
			}
			if (_renderAxis) {
				_gizmo.render(camera);
			}
		}
		if (!depthTest) {
			video::disable(video::State::DepthTest);
		}
		_shapeRenderer.render(_referencePointMesh, camera, _referencePointModelMatrix);
	} else if (!depthTest) {
		video::disable(video::State::DepthTest);
	}
}

void SceneManager::construct() {
	_modifier.construct();
	_mementoHandler.construct();
	_volumeRenderer.construct();

	command::Command::registerCommand("xs", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::error("Usage: xs <lua-generator-script-filename> [help]");
			return;
		}
		const core::String luaScript = _luaGenerator.load(args[0]);
		if (luaScript.empty()) {
			Log::error("Failed to load %s", args[0].c_str());
			return;
		}

		core::DynamicArray<core::String> luaArgs;
		for (size_t i = 1; i < args.size(); ++i) {
			luaArgs.push_back(args[i]);
		}

		if (!runScript(luaScript, luaArgs)) {
			Log::error("Failed to execute %s", args[0].c_str());
		} else {
			Log::info("Executed script %s", args[0].c_str());
		}
	}).setHelp("Executes a lua script to modify the current active volume")
		.setArgumentCompleter(voxelgenerator::scriptCompleter(io::filesystem()));

	core::Var::get(cfg::VoxEditLastPalette, "nippon");
	_modelSpace = core::Var::get(cfg::VoxEditModelSpace, "0");

	for (int i = 0; i < lengthof(DIRECTIONS); ++i) {
		command::Command::registerActionButton(
				core::string::format("movecursor%s", DIRECTIONS[i].postfix),
				_move[i]).setBindingContext(BindingContext::Model);
	}

	command::Command::registerActionButton("zoom_in", _zoomIn).setBindingContext(BindingContext::Editing);
	command::Command::registerActionButton("zoom_out", _zoomOut).setBindingContext(BindingContext::Editing);
	command::Command::registerActionButton("camera_rotate", _rotate).setBindingContext(BindingContext::Editing);
	command::Command::registerActionButton("camera_pan", _pan).setBindingContext(BindingContext::Editing);
	command::Command::registerCommand("mouse_layer_select", [&] (const command::CmdArgs&) {
		if (_sceneModeNodeIdTrace != -1) {
			Log::debug("switch active node to hovered from scene graph mode: %i", _sceneModeNodeIdTrace);
			nodeActivate(_sceneModeNodeIdTrace);
		}
	}).setHelp("Switch active node to hovered from scene graph mode").setBindingContext(BindingContext::Scene);

	command::Command::registerCommand("animation_cycle", [this] (const command::CmdArgs& argv) {
		int offset = 1;
		if (argv.size() > 0) {
			offset = core::string::toInt(argv[0]);
		}
		_animationIdx += offset;
		while (_animationIdx < 0) {
			_animationIdx += (core::enumVal(animation::Animation::MAX) + 1);
		}
		_animationIdx %= (core::enumVal(animation::Animation::MAX) + 1);
		Log::info("current animation idx: %i", _animationIdx);
		animationEntity().setAnimation((animation::Animation)_animationIdx, true);
	}).setHelp("Cycle between all possible animations");

	command::Command::registerCommand("animation_save", [&] (const command::CmdArgs& args) {
		core::String name = "entity";
		if (!args.empty()) {
			name = args[0];
		}
		saveAnimationEntity(name.c_str());
	}).setHelp("Save the animation models and config values");

	command::Command::registerCommand("togglescene", [this] (const command::CmdArgs& args) {
		toggleEditMode();
	}).setHelp("Toggle scene mode on/off");

	command::Command::registerCommand("layerssave", [&] (const command::CmdArgs& args) {
		core::String dir = ".";
		if (!args.empty()) {
			dir = args[0];
		}
		if (!saveModels(dir)) {
			Log::error("Failed to save models to dir: %s", dir.c_str());
		}
	}).setHelp("Save all models into filenames represented by their layer/node names");

	command::Command::registerCommand("layersave", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc < 1) {
			Log::info("Usage: layersave <nodeid> [<file>]");
			return;
		}
		const int nodeId = core::string::toInt(args[0]);
		core::String file = core::string::format("layer%i.vox", nodeId);
		if (args.size() == 2) {
			file = args[1];
		}
		if (!saveNode(nodeId, file)) {
			Log::error("Failed to save node %i to file: %s", nodeId, file.c_str());
		}
	}).setHelp("Save a single model to the given path with their layer/node names");

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
	}).setHelp("Create a new scene (with a given name and width, height, depth - all optional)");

	command::Command::registerCommand("noise", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc != 4) {
			Log::info("Usage: noise <octaves> <lacunarity> <frequency> <gain>");
			return;
		}
		int octaves = core::string::toInt(args[0]);
		float lacunarity = core::string::toFloat(args[0]);
		float frequency = core::string::toFloat(args[0]);
		float gain = core::string::toFloat(args[0]);
		voxelgenerator::noise::NoiseType type = voxelgenerator::noise::NoiseType::ridgedMF;
		noise(octaves, lacunarity, frequency, gain, type);
	}).setHelp("Fill the volume with noise");

	command::Command::registerCommand("crop", [&] (const command::CmdArgs& args) {
		crop();
	}).setHelp("Crop the current layer to the voxel boundaries");

	command::Command::registerCommand("scale", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		int nodeId = activeNode();
		if (argc == 1) {
			nodeId = core::string::toInt(args[0]);
		}
		scale(nodeId);
	}).setHelp("Scale the current layer or given layer down");

	command::Command::registerCommand("colortolayer", [&] (const command::CmdArgs& args) {
		const int argc = (int)args.size();
		if (argc < 1) {
			const voxel::Voxel voxel = _modifier.cursorVoxel();
			colorToNewLayer(voxel);
		} else {
			const uint8_t index = core::string::toInt(args[0]);
			const voxel::Voxel voxel = voxel::createVoxel(voxel::VoxelType::Generic, index);
			colorToNewLayer(voxel);
		}
	}).setHelp("Move the voxels of the current selected palette index or the given index into a new layer");

	command::Command::registerCommand("abortaction", [&] (const command::CmdArgs& args) {
		_modifier.aabbStop();
	}).setHelp("Aborts the current modifier action");

	command::Command::registerCommand("setreferenceposition", [&] (const command::CmdArgs& args) {
		if (args.size() != 3) {
			Log::info("Expected to get x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		setReferencePosition(glm::ivec3(x, y, z));
	}).setHelp("Set the reference position to the specified position");

	command::Command::registerCommand("movecursor", [this] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Expected to get relative x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		moveCursor(x, y, z);
	}).setHelp("Move the cursor by the specified offsets");

	command::Command::registerCommand("loadpalette", [this] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Expected to get the palette NAME as part of palette-NAME.[png|lua]");
			return;
		}
		loadPalette(args[0]);
	}).setHelp("Load an existing palette by name. E.g. 'nippon'");

	command::Command::registerCommand("cursor", [this] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Expected to get x, y and z coordinates");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		setCursorPosition(glm::ivec3(x, y, z), true);
	}).setHelp("Set the cursor to the specified position");

	command::Command::registerCommand("setreferencepositiontocursor", [&] (const command::CmdArgs& args) {
		setReferencePosition(cursorPosition());
	}).setHelp("Set the reference position to the current cursor position").setBindingContext(BindingContext::Model);

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
	}).setHelp("Resize your volume about given x, y and z size");

	command::Command::registerActionButton("shift", _gizmo).setBindingContext(BindingContext::Scene);
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
	}).setHelp("Shift the volume by the given values");

	command::Command::registerCommand("center_referenceposition", [&] (const command::CmdArgs& args) {
		const glm::ivec3& refPos = referencePosition();
		_sceneGraph.foreachGroup([&] (int nodeId) {
			const auto* v = volume(nodeId);
			if (v == nullptr) {
				return;
			}
			const voxel::Region& region = v->region();
			const glm::ivec3& center = region.getCenter();
			const glm::ivec3& delta = refPos - center;
			shift(nodeId, delta);
		});
	}).setHelp("Center the current active layers at the reference position");

	command::Command::registerCommand("center_origin", [&] (const command::CmdArgs& args) {
		_sceneGraph.foreachGroup([&] (int nodeId) {
			const auto* v = volume(nodeId);
			if (v == nullptr) {
				return;
			}
			const voxel::Region& region = v->region();
			const glm::ivec3& delta = -region.getCenter();
			shift(nodeId, delta);
		});
		setReferencePosition(glm::zero<glm::ivec3>());
	}).setHelp("Center the current active layers at the origin");

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
	}).setHelp("Move the voxels inside the volume by the given values");

	command::Command::registerCommand("copy", [&] (const command::CmdArgs& args) {
		copy();
	}).setHelp("Copy selection");

	command::Command::registerCommand("paste", [&] (const command::CmdArgs& args) {
		const Selection& selection = _modifier.selection();
		if (selection.isValid()) {
			paste(selection.getLowerCorner());
		} else {
			paste(_referencePos);
		}
	}).setHelp("Paste clipboard to current selection or reference position");

	command::Command::registerCommand("pastecursor", [&] (const command::CmdArgs& args) {
		paste(_modifier.cursorPosition());
	}).setHelp("Paste clipboard to current cursor position");

	command::Command::registerCommand("cut", [&] (const command::CmdArgs& args) {
		cut();
	}).setHelp("Cut selection");

	command::Command::registerCommand("undo", [&] (const command::CmdArgs& args) {
		undo();
	}).setHelp("Undo your last step");

	command::Command::registerCommand("redo", [&] (const command::CmdArgs& args) {
		redo();
	}).setHelp("Redo your last step");

	command::Command::registerCommand("rotate", [&] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Usage: rotate <x> <y> <z> [rotAroundPivot=false]");
			Log::info("angles are given in degrees");
			Log::info("rotAroundPivot: rotate around pivot (true)");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		bool rotateAroundReferencePosition = false;
		if (args.size() >= 4) {
			rotateAroundReferencePosition = core::string::toBool(args[3]);
		}
		rotate(activeNode(), glm::ivec3(x, y, z), true, rotateAroundReferencePosition);
	}).setHelp("Rotate active layer by the given angles (in degree)");

	command::Command::registerCommand("rotateall", [&] (const command::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Usage: rotateall <x> <y> <z> [rotAroundPivot=false]");
			Log::info("angles are given in degrees");
			Log::info("rotAroundPivot: rotate around pivot (true)");
			return;
		}
		const int x = core::string::toInt(args[0]);
		const int y = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		bool rotateAroundReferencePosition = false;
		if (args.size() >= 4) {
			rotateAroundReferencePosition = core::string::toBool(args[3]);
		}
		rotate(x, y, z, true, rotateAroundReferencePosition);
	}).setHelp("Rotate scene by the given angles (in degree)");

	command::Command::registerCommand("layermerge", [&] (const command::CmdArgs& args) {
		int nodeId1;
		int nodeId2;
		if (args.size() == 2) {
			nodeId1 = core::string::toInt(args[0]);
			nodeId2 = core::string::toInt(args[1]);
		} else {
			nodeId1 = activeNode();
			// FIXME: this layer id might be an empty slot
			nodeId2 = nodeId1 + 1;
		}
		merge(nodeId1, nodeId2);
	}).setHelp("Merge two given layers or active layer with the one below");

	command::Command::registerCommand("layermergeall", [&] (const command::CmdArgs& args) {
		mergeMultiple(LayerMergeFlags::All);
	}).setHelp("Merge all layers");

	command::Command::registerCommand("layermergevisible", [&] (const command::CmdArgs& args) {
		mergeMultiple(LayerMergeFlags::Visible);
	}).setHelp("Merge all visible layers");

	command::Command::registerCommand("layermergelocked", [&] (const command::CmdArgs& args) {
		mergeMultiple(LayerMergeFlags::Locked);
	}).setHelp("Merge all locked layers");

	command::Command::registerCommand("animate", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Usage: animate <framedelaymillis> <0|1>");
			Log::info("framedelay of 0 will stop the animation, too");
			return;
		}
		if (args.size() == 2) {
			if (!core::string::toBool(args[1])) {
				_animationSpeed = 0.0;
				return;
			}
		}
		_animationSpeed = core::string::toDouble(args[0]) / 1000.0;
	}).setHelp("Animate all visible layers with the given delay in millis between the frames");

	command::Command::registerCommand("setcolor", [&] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: setcolor <index>");
			return;
		}
		const uint8_t index = core::string::toInt(args[0]);
		const voxel::Voxel voxel = voxel::createVoxel(voxel::VoxelType::Generic, index);
		_modifier.setCursorVoxel(voxel);
	}).setHelp("Use the given index to select the color from the current palette");

	command::Command::registerCommand("setcolorrgb", [&] (const command::CmdArgs& args) {
		if (args.size() != 3) {
			Log::info("Usage: setcolorrgb <red> <green> <blue> (color range 0-255)");
			return;
		}
		const float red = core::string::toFloat(args[0]);
		const float green = core::string::toFloat(args[1]);
		const float blue = core::string::toFloat(args[2]);
		const glm::vec4 color(red / 255.0f, green / 255.0, blue / 255.0, 1.0f);
		const voxel::MaterialColorArray& materialColors = voxel::getMaterialColors();
		const int index = core::Color::getClosestMatch(color, materialColors);
		const voxel::Voxel voxel = voxel::createVoxel(voxel::VoxelType::Generic, index);
		_modifier.setCursorVoxel(voxel);
	}).setHelp("Set the current selected color by finding the closest rgb match in the palette");

	command::Command::registerCommand("pickcolor", [&] (const command::CmdArgs& args) {
		// during mouse movement, the current cursor position might be at an air voxel (this
		// depends on the mode you are editing in), thus we should use the cursor voxel in
		// that case
		if (_traceViaMouse && !voxel::isAir(_hitCursorVoxel.getMaterial())) {
			_modifier.setCursorVoxel(_hitCursorVoxel);
			return;
		}
		// resolve the voxel via cursor position. This allows to use also get the proper
		// result if we moved the cursor via keys (and thus might have skipped tracing)
		const glm::ivec3& cursorPos = _modifier.cursorPosition();
		const voxel::RawVolume *v = activeVolume();
		const voxel::Voxel& voxel = v->voxel(cursorPos);
		if (!voxel::isAir(voxel.getMaterial())) {
			_modifier.setCursorVoxel(voxel);
		}
	}).setHelp("Pick the current selected color from current cursor voxel");

	command::Command::registerCommand("flip", [&] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: flip <x|y|z>");
			return;
		}
		const math::Axis axis = math::toAxis(args[0]);
		flip(axis);
	}).setHelp("Flip the selected layers around the given axis");

	command::Command::registerCommand("lock", [&] (const command::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: lock <x|y|z>");
			return;
		}
		const math::Axis axis = math::toAxis(args[0]);
		const bool unlock = (_lockedAxis & axis) == axis;
		setLockedAxis(axis, unlock);
	}).setHelp("Toggle locked mode for the given axis at the current cursor position");

	command::Command::registerCommand("lockx", [&] (const command::CmdArgs& args) {
		const math::Axis axis = math::Axis::X;
		const bool unlock = (_lockedAxis & axis) == axis;
		setLockedAxis(axis, unlock);
	}).setHelp("Toggle locked mode for the x axis at the current cursor position");

	command::Command::registerCommand("locky", [&] (const command::CmdArgs& args) {
		const math::Axis axis = math::Axis::Y;
		const bool unlock = (_lockedAxis & axis) == axis;
		setLockedAxis(axis, unlock);
	}).setHelp("Toggle locked mode for the y axis at the current cursor position");

	command::Command::registerCommand("lockz", [&] (const command::CmdArgs& args) {
		const math::Axis axis = math::Axis::Z;
		const bool unlock = (_lockedAxis & axis) == axis;
		setLockedAxis(axis, unlock);
	}).setHelp("Toggle locked mode for the z axis at the current cursor position");

	command::Command::registerCommand("centerplane", [&] (const command::CmdArgs& args) {
		modifier().setCenterMode(!modifier().centerMode());
	}).setHelp("Toggle center plane building");

	command::Command::registerCommand("layeradd", [&] (const command::CmdArgs& args) {
		const char *name = args.size() > 0 ? args[0].c_str() : "";
		const char *width = args.size() > 1 ? args[1].c_str() : "64";
		const char *height = args.size() > 2 ? args[2].c_str() : width;
		const char *depth = args.size() > 3 ? args[3].c_str() : height;
		const int iw = core::string::toInt(width) - 1;
		const int ih = core::string::toInt(height) - 1;
		const int id = core::string::toInt(depth) - 1;
		const voxel::Region region(glm::zero<glm::ivec3>(), glm::ivec3(iw, ih, id));
		if (!region.isValid()) {
			Log::warn("Invalid size provided (%i:%i:%i - %s:%s:%s)", iw, ih, id, width, height, depth);
			return;
		}
		voxel::SceneGraphNode newNode;
		newNode.setVolume(new voxel::RawVolume(region), true);
		newNode.setName(name);
		const int parentId = activeNode();
		addNodeToSceneGraph(newNode, parentId);
	}).setHelp("Add a new layer (with a given name and width, height, depth - all optional)");

	command::Command::registerCommand("layerdelete", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			nodeRemove(*node);
		}
	}).setHelp("Delete a particular node by id - or the current active one");

	command::Command::registerCommand("layerlock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(true);
		}
	}).setHelp("Lock a particular layer by id - or the current active one");

	command::Command::registerCommand("togglelayerlock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(!node->locked());
		}
	}).setHelp("Toggle the lock state of a particular layer by id - or the current active one");

	command::Command::registerCommand("layerunlock", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode* node = sceneGraphNode(nodeId)) {
			node->setLocked(false);
		}
	}).setHelp("Unlock a particular layer by id - or the current active one");

	command::Command::registerCommand("layeractive", [&] (const command::CmdArgs& args) {
		if (args.empty()) {
			Log::info("Active node: %i", activeNode());
			return;
		}
		const int nodeId = core::string::toInt(args[0]);
		nodeActivate(nodeId);
	}).setHelp("Set or print the current active layer");

	command::Command::registerCommand("togglelayerstate", [&](const command::CmdArgs &args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			node->setVisible(!node->visible());
		}
	}).setHelp("Toggle the visible state of a layer");

	command::Command::registerCommand("layerhideall", [&](const command::CmdArgs &args) {
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			node.setVisible(false);
		}
	}).setHelp("Hide all layers");

	command::Command::registerCommand("layerlockall", [&](const command::CmdArgs &args) {
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			node.setLocked(true);
		}
	}).setHelp("Lock all layers");

	command::Command::registerCommand("layerunlockall", [&] (const command::CmdArgs& args) {
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			node.setLocked(false);
		}
	}).setHelp("Unlock all layers");

	command::Command::registerCommand("layerhideothers", [&] (const command::CmdArgs& args) {
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			if (node.id() == activeNode()) {
				node.setVisible(true);
				continue;
			}
			node.setVisible(false);
		}
	}).setHelp("Hide all layers except the active one");

	command::Command::registerCommand("layerrename", [&] (const command::CmdArgs& args) {
		if (args.size() == 1) {
			const int nodeId = activeNode();
			if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
				nodeRename(*node, args[0]);
			}
		} else if (args.size() == 2) {
			const int nodeId = core::string::toInt(args[0]);
			if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
				nodeRename(*node, args[1]);
			}
		} else {
			Log::info("Usage: layerrename [<nodeid>] newname");
		}
	}).setHelp("Rename the current node or the given node id");

	command::Command::registerCommand("layershowall", [&] (const command::CmdArgs& args) {
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			node.setVisible(true);
		}
	}).setHelp("Show all layers");

	command::Command::registerCommand("layerduplicate", [&] (const command::CmdArgs& args) {
		const int nodeId = args.size() > 0 ? core::string::toInt(args[0]) : activeNode();
		if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
			nodeDuplicate(*node);
		}
	}).setHelp("Duplicates the current node or the given node id");

	_grayScale = core::Var::get(cfg::VoxEditGrayInactive, "false");
	_grayScale->setHelp("Render the inactive layers in gray scale mode");
	core::Var::get(cfg::VoxformatMergequads, "true", core::CV_NOPERSIST)->setHelp("Merge similar quads to optimize the mesh");
	core::Var::get(cfg::VoxformatReusevertices, "true", core::CV_NOPERSIST)->setHelp("Reuse vertices or always create new ones");
	core::Var::get(cfg::VoxformatAmbientocclusion, "false", core::CV_NOPERSIST)->setHelp("Extra vertices for ambient occlusion");
	core::Var::get(cfg::VoxformatScale, "1.0", core::CV_NOPERSIST)->setHelp("Scale the vertices by the given factor");
	core::Var::get(cfg::VoxformatQuads, "true", core::CV_NOPERSIST)->setHelp("Export as quads. If this false, triangles will be used.");
	core::Var::get(cfg::VoxformatWithcolor, "true", core::CV_NOPERSIST)->setHelp("Export with vertex colors");
	core::Var::get(cfg::VoxformatWithtexcoords, "true", core::CV_NOPERSIST)->setHelp("Export with uv coordinates of the palette image");
	core::Var::get("palette", voxel::getDefaultPaletteName())->setHelp("This is the NAME part of palette-<NAME>.png or absolute png file to use (1x256)");
}

void SceneManager::flip(math::Axis axis) {
	_sceneGraph.foreachGroup([&] (int nodeId) {
		auto* v = volume(nodeId);
		if (v == nullptr) {
			return;
		}
		voxel::RawVolume* newVolume = voxel::mirrorAxis(v, axis);
		voxel::Region r = newVolume->region();
		r.accumulate(v->region());
		if (!setNewVolume(nodeId, newVolume)) {
			delete newVolume;
			return;
		}
		modified(nodeId, r);
	});
}

void SceneManager::toggleEditMode() {
	if (_editMode == EditMode::Model) {
		_editMode = EditMode::Scene;
		_modifier.aabbStop();
	} else if (_editMode == EditMode::Scene) {
		_editMode = EditMode::Model;
		_gizmo.resetMode();
	}
	setGizmoPosition();
	// don't abort or toggle any other mode
}

void SceneManager::setVoxelsForCondition(std::function<voxel::Voxel()> voxel, std::function<bool(const voxel::Voxel&)> condition) {
	_sceneGraph.foreachGroup([&] (int nodeId) {
		auto* v = volume(nodeId);
		if (v == nullptr) {
			return;
		}
		voxel::RawVolumeWrapper wrapper(v);
		const Selection& selection = _modifier.selection();
		if (selection.isValid()) {
			wrapper.setRegion(selection);
		}
		const int cnt = voxelutil::visitVolume(wrapper, [&] (int32_t x, int32_t y, int32_t z, const voxel::Voxel&) {
			v->setVoxel(x, y, z, voxel());
		}, condition);
		if (cnt > 0) {
			modified(nodeId, wrapper.dirtyRegion());
			Log::debug("Modified %i voxels", cnt);
		}
	});
}

bool SceneManager::init() {
	++_initialized;
	if (_initialized > 1) {
		Log::debug("Already initialized");
		return true;
	}

	const char *paletteName = core::Var::getSafe(cfg::VoxEditLastPalette)->strVal().c_str();
	const io::FilesystemPtr& filesystem = io::filesystem();
	const io::FilePtr& paletteFile = filesystem->open(core::string::format("palette-%s.png", paletteName));
	const io::FilePtr& luaFile = filesystem->open(core::string::format("palette-%s.lua", paletteName));
	if (!voxel::initMaterialColors(paletteFile, luaFile)) {
		Log::warn("Failed to initialize the palette data for %s, falling back to default", paletteName);
		if (!voxel::initDefaultMaterialColors()) {
			Log::error("Failed to initialize the palette data");
			return false;
		}
	}

	if (!_gizmo.init()) {
		Log::error("Failed to initialize the gizmo");
		return false;
	}
	if (!_mementoHandler.init()) {
		Log::error("Failed to initialize the memento handler");
		return false;
	}
	if (!_volumeRenderer.init()) {
		Log::error("Failed to initialize the volume renderer");
		return false;
	}
	if (!_shapeRenderer.init()) {
		Log::error("Failed to initialize the shape renderer");
		return false;
	}
	if (!_gridRenderer.init()) {
		Log::error("Failed to initialize the grid renderer");
		return false;
	}
	if (!_modifier.init()) {
		Log::error("Failed to initialize the modifier");
		return false;
	}
	if (!_volumeCache.init()) {
		Log::error("Failed to initialize the volume cache");
		return false;
	}
	if (!_animationSystem.init()) {
		Log::error("Failed to initialize the animation system");
		return false;
	}
	if (!_animationRenderer.init()) {
		Log::error("Failed to initialize the character renderer");
		return false;
	}
	_animationRenderer.setClearColor(core::Color::Clear);
	const auto& meshCache = core::make_shared<voxelformat::MeshCache>();
	_animationCache = core::make_shared<animation::AnimationCache>(meshCache);
	if (!_animationCache->init()) {
		Log::error("Failed to initialize the character mesh cache");
		return false;
	}

	if (!_luaGenerator.init()) {
		Log::error("Failed to initialize the lua generator bindings");
		return false;
	}

	_autoSaveSecondsDelay = core::Var::get(cfg::VoxEditAutoSaveSeconds, "180");
	_ambientColor = core::Var::get(cfg::VoxEditAmbientColor, "0.2 0.2 0.2");
	_diffuseColor = core::Var::get(cfg::VoxEditDiffuseColor, "1.0 1.0 1.0");
	_cameraZoomSpeed = core::Var::get(cfg::VoxEditCameraZoomSpeed, "10.0");
	const core::TimeProviderPtr& timeProvider = app::App::getInstance()->timeProvider();
	_lastAutoSave = timeProvider->tickSeconds();

	for (int i = 0; i < lengthof(_planeMeshIndex); ++i) {
		_planeMeshIndex[i] = -1;
	}

	_shapeBuilder.clear();
	_shapeBuilder.setColor(core::Color::alpha(core::Color::SteelBlue, 0.8f));
	_shapeBuilder.sphere(8, 6, 0.5f);
	_referencePointMesh = _shapeRenderer.create(_shapeBuilder);

	_lockedAxis = math::Axis::None;
	return true;
}

bool SceneManager::runScript(const core::String& script, const core::DynamicArray<core::String>& args) {
	const int nodeId = activeNode();
	voxel::RawVolume* volume = this->volume(nodeId);
	const Selection& selection = _modifier.selection();
	voxel::RawVolumeWrapper wrapper(volume);
	if (selection.isValid()) {
		wrapper.setRegion(selection);
	}
	const bool retVal = _luaGenerator.exec(script, &wrapper, wrapper.region(), _modifier.cursorVoxel(), args);
	modified(nodeId, wrapper.dirtyRegion());
	return retVal;
}

bool SceneManager::animateActive() const {
	return _animationSpeed > 0.0;
}

void SceneManager::animate(double nowSeconds) {
	if (!animateActive()) {
		return;
	}
	if (_nextFrameSwitch <= nowSeconds) {
		_nextFrameSwitch = nowSeconds + _animationSpeed;
		const int modelCount = (int)_sceneGraph.size(voxel::SceneGraphNodeType::Model);
		const int roundTrip = modelCount + _currentAnimationLayer;
		for (int modelIdx = _currentAnimationLayer + 1; modelIdx < roundTrip; ++modelIdx) {
			voxel::SceneGraphNode *node = _sceneGraph[_currentAnimationLayer];
			core_assert_always(node != nullptr);
			node->setVisible(false);
			_currentAnimationLayer = modelIdx % modelCount;
			node = _sceneGraph[_currentAnimationLayer];
			core_assert_always(node != nullptr);
			node->setVisible(true);
			return;
		}
	}
}

void SceneManager::zoom(video::Camera& camera, float level) const {
	const float cameraSpeed = _cameraZoomSpeed->floatVal();
	const float value = cameraSpeed * level;
	camera.zoom(value);
}

void SceneManager::update(double nowSeconds) {
	_volumeRenderer.update();
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

	if (_camera != nullptr) {
		if (_editMode == EditMode::Scene) {
			_gizmo.setModelSpace();
			setGizmoPosition();
		} else if (_modelSpace->boolVal() != _gizmo.isModelSpace()) {
			const bool newModelSpaceState = _modelSpace->boolVal();
			if (newModelSpaceState) {
				Log::info("switch to model space");
				_gizmo.setModelSpace();
			} else {
				Log::info("switch to world space");
				_gizmo.setWorldSpace();
			}
			setGizmoPosition();
		}

		if (_editMode == EditMode::Scene) {
			_gizmo.updateMode(*_camera, _mouseCursor);
			_gizmo.execute(nowSeconds, [&] (const glm::vec3& deltaMovement, render::GizmoMode mode) {
				executeGizmoAction(glm::ivec3(deltaMovement), mode);
			});
		}
	}
	if (_ambientColor->isDirty()) {
		_volumeRenderer.setAmbientColor(_ambientColor->vec3Val());
		_ambientColor->markClean();
	}
	if (_diffuseColor->isDirty()) {
		_volumeRenderer.setDiffuseColor(_diffuseColor->vec3Val());
		_diffuseColor->markClean();
	}
	animate(nowSeconds);
	autosave();
}

void SceneManager::shutdown() {
	if (_initialized > 0) {
		--_initialized;
	}
	if (_initialized != 0) {
		return;
	}

	if (_copy) {
		delete _copy;
		_copy = nullptr;
	}

	_volumeRenderer.shutdown();
	_sceneGraph.clear();

	_luaGenerator.shutdown();
	_volumeCache.shutdown();
	_mementoHandler.shutdown();
	_modifier.shutdown();
	_gizmo.shutdown();
	_shapeRenderer.shutdown();
	_shapeBuilder.shutdown();
	_gridRenderer.shutdown();
	_mementoHandler.clearStates();
	_animationRenderer.shutdown();
	if (_animationCache) {
		_animationCache->shutdown();
	}
	_character.shutdown();
	_bird.shutdown();
	_animationSystem.shutdown();

	_referencePointMesh = -1;
	_aabbMeshIndex = -1;

	command::Command::unregisterActionButton("zoom_in");
	command::Command::unregisterActionButton("zoom_out");
	command::Command::unregisterActionButton("camera_rotate");
	command::Command::unregisterActionButton("camera_pan");
}

animation::AnimationEntity& SceneManager::animationEntity() {
	if (_entityType == animation::AnimationSettings::Type::Character) {
		return _character;
	}
	return _bird;
}

bool SceneManager::saveAnimationEntity(const char *name) {
	_dirty = false;
	// TODO: race and gender
	const core::String& chrName = core::string::format("chr/human-male-%s", name);
	const core::String& luaFilePath = animation::luaFilename(chrName.c_str());
	const core::String luaDir(core::string::extractPath(luaFilePath));
	io::filesystem()->createDir(luaDir);
	const io::FilePtr& luaFile = io::filesystem()->open(luaFilePath, io::FileMode::SysWrite);
	const animation::AnimationSettings& animSettings = animationEntity().animationSettings();
	if (saveAnimationEntityLua(animSettings, animationEntity().skeletonAttributes(), name, luaFile)) {
		Log::info("Wrote lua script: %s", luaFile->name().c_str());
	} else {
		Log::error("Failed to write lua script: %s", luaFile->name().c_str());
	}

	const int mountCount = (int)_sceneGraph.size();
	for (int i = 0; i < mountCount; ++i) {
		voxel::SceneGraphNode *node = _sceneGraph[i];
		core_assert_always(node != nullptr);
		const voxel::RawVolume* v = node->volume();
		if (v == nullptr) {
			continue;
		}
		const core::String& value = node->property("type");
		if (value.empty()) {
			const core::String& unknown = core::string::format("%i-%s-%s.vox", (int)i, node->name().c_str(), name);
			Log::warn("No type metadata found on layer %i. Saving to %s", (int)i, unknown.c_str());
			if (!saveNode((int)i, unknown)) {
				Log::warn("Failed to save unknown layer to %s", unknown.c_str());
				_dirty = true;
			}
			continue;
		}
		const int characterMeshTypeId = core::string::toInt(value);
		const core::String& fullPath = animSettings.fullPath(characterMeshTypeId, name);
		if (!saveNode((int)i, fullPath)) {
			Log::warn("Failed to save type %i to %s", characterMeshTypeId, fullPath.c_str());
			_dirty = true;
		}
	}

	return true;
}

bool SceneManager::loadAnimationEntity(const core::String& luaFile) {
	const core::String& lua = io::filesystem()->load(luaFile);
	animation::AnimationSettings settings;
	if (!animation::loadAnimationSettings(lua, settings, nullptr)) {
		Log::warn("Failed to initialize the animation settings for %s", luaFile.c_str());
		return false;
	}
	_entityType = settings.type();
	if (_entityType == animation::AnimationSettings::Type::Max) {
		Log::warn("Failed to detect the entity type for %s", luaFile.c_str());
		return false;
	}

	if (!animationEntity().initSettings(lua)) {
		Log::warn("Failed to initialize the animation settings and attributes for %s", luaFile.c_str());
	}

	voxel::SceneGraph newSceneGraph;
	if (!_volumeCache.getVolumes(animationEntity().animationSettings(), newSceneGraph)) {
		return false;
	}

	loadSceneGraph(newSceneGraph);
	_animationUpdate = true;
	_editMode = EditMode::Animation;

	animationEntity().setAnimation(animation::Animation::IDLE, true);

	return true;
}

bool SceneManager::extractVolume() {
	core_trace_scoped(SceneManagerExtract);
	const size_t n = _extractRegions.size();
	if (n <= 0) {
		return false;
	}
	Log::debug("Extract the meshes for %i regions", (int)n);
	for (size_t i = 0; i < n; ++i) {
		const voxel::Region& region = _extractRegions[i].region;
		if (voxel::SceneGraphNode* node = sceneGraphNode(_extractRegions[i].nodeId)) {
			if (!_volumeRenderer.extractRegion(*node, region)) {
				Log::error("Failed to extract the model mesh");
			}
			Log::debug("Extract node %i", _extractRegions[i].nodeId);
			voxel::logRegion("Extraction", region);
		}
	}
	_extractRegions.clear();
	return true;
}

void SceneManager::noise(int octaves, float lacunarity, float frequency, float gain, voxelgenerator::noise::NoiseType type) {
	math::Random random;
	const int nodeId = activeNode();
	voxel::RawVolumeWrapper wrapper(volume(nodeId));
	if (voxelgenerator::noise::generate(wrapper, octaves, lacunarity, frequency, gain, type, random) <= 0) {
		Log::warn("Could not generate noise");
		return;
	}
	// if the same noise is generated again - the wrapper doesn't override anything, but the voxels are still somehow placed.
	if (!wrapper.dirtyRegion().isValid()) {
		return;
	}
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::lsystem(const core::String &axiom, const core::DynamicArray<voxelgenerator::lsystem::Rule> &rules, float angle, float length,
		float width, float widthIncrement, int iterations, float leavesRadius) {
	math::Random random;
	const int nodeId = activeNode();
	voxel::RawVolumeWrapper wrapper(volume(nodeId));
	voxelgenerator::lsystem::generate(wrapper, referencePosition(), axiom, rules, angle, length, width, widthIncrement, iterations, random, leavesRadius);
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::createTree(const voxelgenerator::TreeContext& ctx) {
	math::Random random(ctx.cfg.seed);
	const int nodeId = activeNode();
	voxel::RawVolumeWrapper wrapper(volume(nodeId));
	voxelgenerator::tree::createTree(wrapper, ctx, random);
	modified(nodeId, wrapper.dirtyRegion());
}

void SceneManager::setReferencePosition(const glm::ivec3& pos) {
	_referencePos = pos;
	const glm::vec3 posAligned((float)_referencePos.x + 0.5f, (float)_referencePos.y + 0.5f, (float)_referencePos.z + 0.5f);
	_referencePointModelMatrix = glm::translate(posAligned);
}

void SceneManager::moveCursor(int x, int y, int z) {
	glm::ivec3 p = cursorPosition();
	const int res = gridRenderer().gridResolution();
	p.x += x * res;
	p.y += y * res;
	p.z += z * res;
	setCursorPosition(p, true);
	const voxel::RawVolume *v = activeVolume();
	_hitCursorVoxel = v->voxel(cursorPosition());
	_traceViaMouse = false;
}

void SceneManager::setCursorPosition(glm::ivec3 pos, bool force) {
	const voxel::RawVolume* v = volume(activeNode());
	if (v == nullptr) {
		return;
	}

	const int res = gridRenderer().gridResolution();
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
	const glm::ivec3& oldCursorPos = cursorPosition();
	if (!force) {
		if ((_lockedAxis & math::Axis::X) != math::Axis::None) {
			pos.x = oldCursorPos.x;
		}
		if ((_lockedAxis & math::Axis::Y) != math::Axis::None) {
			pos.y = oldCursorPos.y;
		}
		if ((_lockedAxis & math::Axis::Z) != math::Axis::None) {
			pos.z = oldCursorPos.z;
		}
	}

	if (!region.containsPoint(pos)) {
		pos = region.moveInto(pos.x, pos.y, pos.z);
	}
	if (oldCursorPos == pos) {
		return;
	}
	_modifier.setCursorPosition(pos, _result.hitFace);

	updateLockedPlane(math::Axis::X);
	updateLockedPlane(math::Axis::Y);
	updateLockedPlane(math::Axis::Z);
}

bool SceneManager::trace(bool force, voxel::PickResult *result) {
	if (result) {
		*result = _result;
	}
	if (_editMode == EditMode::Scene) {
		if (_sceneModeNodeIdTrace != -1) {
			// if the trace is not forced, and the mouse cursor position did not change, don't
			// re-execute the trace.
			if (_lastRaytraceX == _mouseCursor.x && _lastRaytraceY == _mouseCursor.y && !force) {
				return true;
			}
		}
		_sceneModeNodeIdTrace = -1;
		core_trace_scoped(EditorSceneOnProcessUpdateRay);
		_lastRaytraceX = _mouseCursor.x;
		_lastRaytraceY = _mouseCursor.y;
		float intersectDist = _camera->farPlane();
		const math::Ray& ray = _camera->mouseRay(_mouseCursor);
		for (voxel::SceneGraphNode &node : _sceneGraph) {
			if (!node.visible()) {
				continue;
			}
			const voxel::Region& region = node.region();
			float distance = 0.0f;
			const math::AABB<float>& aabb = toAABB(region);
			if (aabb.intersect(ray.origin, ray.direction, _camera->farPlane(), distance)) {
				if (distance < intersectDist) {
					intersectDist = distance;
					_sceneModeNodeIdTrace = node.id();
				}
			}
		}
		Log::trace("Hovered node: %i", _sceneModeNodeIdTrace);
		return true;
	} else if (_editMode != EditMode::Model) {
		return false;
	}

	// mouse tracing is disabled - e.g. because the voxel cursor was moved by keyboard
	// shortcuts in this case the execution of the modifier would result in a
	// re-execution of the trace. And that would move the voxel cursor to the mouse pos
	if (!_traceViaMouse) {
		return false;
	}
	// if the trace is not forced, and the mouse cursor position did not change, don't
	// re-execute the trace.
	if (_lastRaytraceX == _mouseCursor.x && _lastRaytraceY == _mouseCursor.y && !force) {
		return true;
	}
	if (_camera == nullptr) {
		return false;
	}
	const voxel::RawVolume* v = activeVolume();
	Log::trace("Execute new trace for %i:%i (%i:%i)",
			_mouseCursor.x, _mouseCursor.y, _lastRaytraceX, _lastRaytraceY);

	core_trace_scoped(EditorSceneOnProcessUpdateRay);
	_lastRaytraceX = _mouseCursor.x;
	_lastRaytraceY = _mouseCursor.y;

	const math::Ray& ray = _camera->mouseRay(_mouseCursor);
	float rayLength = _camera->farPlane();
	const glm::vec3& dirWithLength = ray.direction * rayLength;
	static constexpr voxel::Voxel air;

	_result.didHit = false;
	_result.validPreviousPosition = false;
	_result.firstValidPosition = false;
	_result.direction = ray.direction;
	_result.hitFace = voxel::FaceNames::Max;
	raycastWithDirection(v, ray.origin, dirWithLength, [&] (voxel::RawVolume::Sampler& sampler) {
		if (!_result.firstValidPosition && sampler.currentPositionValid()) {
			_result.firstPosition = sampler.position();
			_result.firstValidPosition = true;
		}

		if (sampler.voxel() != air) {
			_result.didHit = true;
			_result.hitVoxel = sampler.position();
			_result.hitFace = voxel::raycastFaceDetection(ray.origin, ray.direction, _result.hitVoxel, 0.0f, 1.0f);
			return false;
		}
		if (sampler.currentPositionValid()) {
			// while having an axis locked, we should end the trace if we hit the plane
			if (_lockedAxis != math::Axis::None) {
				const glm::ivec3& cursorPos = cursorPosition();
				if ((_lockedAxis & math::Axis::X) != math::Axis::None) {
					if (sampler.position()[0] == cursorPos[0]) {
						return false;
					}
				}
				if ((_lockedAxis & math::Axis::Y) != math::Axis::None) {
					if (sampler.position()[1] == cursorPos[1]) {
						return false;
					}
				}
				if ((_lockedAxis & math::Axis::Z) != math::Axis::None) {
					if (sampler.position()[2] == cursorPos[2]) {
						return false;
					}
				}
			}

			_result.validPreviousPosition = true;
			_result.previousPosition = sampler.position();
		}
		return true;
	});

	if (_modifier.modifierTypeRequiresExistingVoxel()) {
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

	if (_result.didHit) {
		_hitCursorVoxel = v->voxel(_result.hitVoxel);
	}

	if (result) {
		*result = _result;
	}

	return true;
}

void SceneManager::updateLockedPlane(math::Axis axis) {
	if (axis == math::Axis::None) {
		return;
	}
	const int index = math::getIndexForAxis(axis);
	int32_t& meshIndex = _planeMeshIndex[index];
	if ((_lockedAxis & axis) == math::Axis::None) {
		if (meshIndex != -1) {
			_shapeRenderer.deleteMesh(meshIndex);
			meshIndex = -1;
		}
		return;
	}

	const glm::vec4 colors[] = {
		core::Color::LightRed,
		core::Color::LightGreen,
		core::Color::LightBlue
	};
	updateShapeBuilderForPlane(_shapeBuilder, _sceneGraph.region(), false, cursorPosition(), axis, core::Color::alpha(colors[index], 0.4f));
	_shapeRenderer.createOrUpdate(meshIndex, _shapeBuilder);
}

void SceneManager::setLockedAxis(math::Axis axis, bool unlock) {
	if (unlock) {
		_lockedAxis &= ~axis;
	} else {
		_lockedAxis |= axis;
	}
	updateLockedPlane(math::Axis::X);
	updateLockedPlane(math::Axis::Y);
	updateLockedPlane(math::Axis::Z);
}

void SceneManager::setGizmoPosition() {
	if (_gizmo.isModelSpace()) {
		const voxel::RawVolume *v = activeVolume();
		const voxel::Region& region = v->region();
		_gizmo.setPosition(region.getLowerCornerf());
	} else {
		_gizmo.setPosition(glm::zero<glm::vec3>());
	}
}

void SceneManager::nodeRename(int nodeId, const core::String &name) {
	if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		nodeRename(*node, name);
	}
}

void SceneManager::nodeRename(voxel::SceneGraphNode &node, const core::String &name) {
	_mementoHandler.markUndo(node.parent(), node.id(), node.name(), nullptr, MementoType::LayerRenamed);
	node.setName(name);
}

void SceneManager::nodeSetVisible(int nodeId, bool visible) {
	if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		node->setVisible(visible);
	}
}

void SceneManager::nodeSetLocked(int nodeId, bool visible) {
	if (voxel::SceneGraphNode *node = sceneGraphNode(nodeId)) {
		node->setLocked(visible);
	}
}

void SceneManager::nodeRemove(int nodeId) {
	if (_sceneGraph.hasNode(nodeId)) {
		nodeRemove(_sceneGraph.node(nodeId));
	}
}

void SceneManager::nodeRemove(voxel::SceneGraphNode &node) {
	const int nodeId = node.id();
	Log::debug("Delete node %i with name %s", nodeId, node.name().c_str());
	_mementoHandler.markNodeDeleted(node.parent(), nodeId, node.name(), node.volume());
	_sceneGraph.removeNode(node.id());
	_needAutoSave = true;
	_dirty = true;
	updateAABBMesh();
}

void SceneManager::nodeDuplicate(voxel::SceneGraphNode &node) {
	if (node.type() == voxel::SceneGraphNodeType::Root) {
		return;
	}
	const voxel::RawVolume* v = node.volume();
	voxel::SceneGraphNode newNode;
	newNode.setVolume(new voxel::RawVolume(v), true);
	newNode.setName(node.name());
	newNode.setPivot(node.pivot());
	newNode.setVisible(node.visible());
	newNode.setLocked(node.locked());
	newNode.addProperties(node.properties());
	/* int newNodeId = */sceneMgr().addNodeToSceneGraph(newNode, node.parent());
	#if 0
	for (int childNodeId : node.children()) {
		// TODO: duplicate children
	}
	#endif
}

void SceneManager::nodeForeachGroup(const std::function<void(int)>& f) {
	_sceneGraph.foreachGroup(f);
}

void SceneManager::nodeActivate(int nodeId) {
	if (!_sceneGraph.hasNode(nodeId)) {
		Log::warn("Given node id %i doesn't exist", nodeId);
		return;
	}
	Log::debug("Activate node %i", nodeId);
	voxel::SceneGraphNode &node = _sceneGraph.node(nodeId);
	if (node.type() != voxel::SceneGraphNodeType::Model) {
		Log::warn("Given node id %i is no model node", nodeId);
		return;
	}
	_sceneGraph.setActiveNode(nodeId);
	const voxel::Region& region = node.region();
	updateGridRenderer(region);
	updateAABBMesh();
	if (!region.containsPoint(referencePosition())) {
		setReferencePosition(node.pivot());
	}
	if (!region.containsPoint(cursorPosition())) {
		setCursorPosition(node.region().getCenter());
	}
	setGizmoPosition();
	resetLastTrace();
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

}
