/**
 * @file
 */

#pragma once

#include "ui/Panel.h"
#include "core/String.h"
#include "voxedit-util/SceneManager.h"

namespace voxedit {

/**
 * @brief Status bar on to the bottom of the main window
 */
class StatusBar : public ui::Panel {
private:
	using Super = ui::Panel;
	SceneManagerPtr _sceneMgr;

public:
	StatusBar(ui::IMGUIApp *app, const SceneManagerPtr &sceneMgr) : Super(app, "statusbar"), _sceneMgr(sceneMgr) {
	}
	void update(const char *title, float height, const core::String &lastExecutedCommand);
#ifdef IMGUI_ENABLE_TEST_ENGINE
	void registerUITests(ImGuiTestEngine *engine, const char *title) override;
#endif
};

}
