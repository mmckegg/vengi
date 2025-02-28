/**
 * @file
 */

#include "AbstractFormatTest.h"

namespace voxelformat {

class VMaxFormatTest : public AbstractFormatTest {};

TEST_F(VMaxFormatTest, testLoad0) {
	// Node 'snapshots' is empty - this scene doesn't contain anything
	testLoad("0voxel.vmax.zip", 0);
}

TEST_F(VMaxFormatTest, testLoad1) {
	testLoad("1voxel.vmax.zip");
}

TEST_F(VMaxFormatTest, testLoad2) {
	testLoad("2voxel.vmax.zip");
}

TEST_F(VMaxFormatTest, testLoad5) {
	testLoad("5voxel.vmax.zip");
}

} // namespace voxelformat
