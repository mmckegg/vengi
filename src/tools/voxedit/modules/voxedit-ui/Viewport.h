/**
 * @file
 */

#pragma once

#include "command/CommandHandler.h"
#include "video/Camera.h"
#include "video/FrameBuffer.h"
#include "imgui.h"
#include "video/WindowedApp.h"
#include "video/gl/GLTypes.h"
#include "video/Camera.h"
#include "video/FrameBuffer.h"
#include "voxel/Region.h"
#include "voxelformat/SceneGraphNode.h"
#include "voxelrender/RawVolumeRenderer.h"

namespace voxelformat {
class SceneGraphNode;
}

struct ImGuiDockNode;

namespace voxedit {

class Viewport {
private:
	enum class SceneCameraMode : uint8_t {
		Free, Top, Left, Front, Max
	};
	static constexpr const char *SceneCameraModeStr[] = {"Free", "Top", "Left", "Front"};
	static_assert(lengthof(SceneCameraModeStr) == (int)SceneCameraMode::Max, "Array size doesn't match enum values");
	const int _id;
	const core::String _uiId;
	const bool _detailedTitle;
	bool _hovered = false;
	SceneCameraMode _camMode = SceneCameraMode::Free;

	/**
	 * while we are still modifying the transform or shifting the volume we don't want to
	 * flood the memento states - thus we lock the memento handler and track this here.
	 * @sa lock()
	 * @sa unlock()
	 * @sa reset()
	 */
	bool _transformMementoLocked = false;
	/**
	 * @sa lock()
	 * @sa unlock()
	 */
	void reset();
	/**
	 * @param[in] keyFrameIdx Only given when we are modifying a transform in scene mode - in edit mode this
	 * should be @c InvalidKeyFrame
	 * @sa lock()
	 * @sa reset()
	 */
	void unlock(const voxelformat::SceneGraphNode &node, voxelformat::KeyFrameIndex keyFrameIdx = InvalidKeyFrame);
	/**
	 * @param[in] keyFrameIdx Only given when we are modifying a transform in scene mode - in edit mode this
	 * should be @c InvalidKeyFrame
	 * @sa unlock()
	 * @sa reset()
	 */
	void lock(const voxelformat::SceneGraphNode &node, voxelformat::KeyFrameIndex keyFrameIdx = InvalidKeyFrame);

	int _mouseX = 0;
	int _mouseY = 0;

	struct Bounds {
		glm::vec3 mins{0};
		glm::vec3 maxs{0};
	};
	Bounds _boundsNode;
	Bounds _bounds;

	voxelrender::RenderContext _renderContext;
	video::Camera _camera;

	core::VarPtr _showAxisVar;
	core::VarPtr _guizmoRotation;
	core::VarPtr _guizmoAllowAxisFlip;
	core::VarPtr _guizmoSnap;
	core::VarPtr _guizmoBounds;
	core::VarPtr _modelGuizmo;
	core::VarPtr _viewDistance;
	core::VarPtr _simplifiedView;
	core::VarPtr _rotationSpeed;

	void renderToFrameBuffer();
	bool setupFrameBuffer(const glm::ivec2& frameBufferSize);
	void handleGuizmo(const voxelformat::SceneGraphNode &node, voxelformat::KeyFrameIndex keyFrameIdx, const glm::mat4 &localMatrix);
	/**
	 * See the return value documentation of @c renderGizmo()
	 * @sa renderGizmo()
	 */
	bool renderSceneAndModelGuizmo(const video::Camera &camera);
	void renderCameraManipulator(video::Camera &camera, float headerSize);
	/**
	 * @return @c true if the the guizmo was used in edit mode.
	 * @note This does not return @c true if the guizmo was used in scene mode. This is due to the fact that the edit
	 * mode volume trace has to be reset when we activate the guizmo. Otherwise you would span an aabb for the modifier
	 * to get executed in
	 */
	bool renderGizmo(video::Camera &camera, float headerSize, const ImVec2 &size);
	void updateViewportTrace(float headerSize);
	bool isFixedCamera() const;
	void renderViewportImage(const glm::ivec2 &contentSize);
	void dragAndDrop(float headerSize);
	void renderBottomBar();
	void renderViewport();
	void renderMenuBar(command::CommandExecutionListener *listener);
	void menuBarCameraMode();
	void menuBarCameraProjection();
	void resetCamera(const glm::vec3 &pos, float distance, const voxel::Region &region);
	void resize(const glm::ivec2& frameBufferSize);
	void move(bool pan, bool rotate, int x, int y);
public:
	Viewport(int id, bool sceneMode, bool detailedTitle = true);
	~Viewport();

	static core::String viewportId(int id);

	void toggleScene();
	bool isSceneMode() const;

	video::Camera& camera();

	bool isHovered() const;
	/**
	 * Update the ui
	 */
	void update(command::CommandExecutionListener *listener);
	bool init();
	void shutdown();

	int id() const;

	void resetCamera();
	bool saveImage(const char* filename);
};

inline video::Camera& Viewport::camera() {
	return _camera;
}

inline int Viewport::id() const {
	return _id;
}

inline bool Viewport::isHovered() const {
	return _hovered;
}

}
