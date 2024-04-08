/**
 * @file
 */

#include "../AnimationPanel.h"
#include "voxedit-util/SceneManager.h"

namespace voxedit {

void AnimationPanel::registerUITests(ImGuiTestEngine *engine, const char *title) {
#if 0
	ImGuiTest *test = IM_REGISTER_TEST(engine, testCategory(), testName());
	test->TestFunc = [=](ImGuiTestContext *ctx) {
		ctx->SetRef(title);
		focusWindow(ctx, title);
	};
#endif
}

} // namespace voxedit
