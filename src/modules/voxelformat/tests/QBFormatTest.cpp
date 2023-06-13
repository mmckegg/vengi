/**
 * @file
 */

#include "AbstractVoxFormatTest.h"
#include "voxelformat/QBFormat.h"
#include "voxelformat/VolumeFormat.h"

namespace voxelformat {

class QBFormatTest: public AbstractVoxFormatTest {
};

TEST_F(QBFormatTest, testLoad) {
	canLoad("qubicle.qb", 10);
}

TEST_F(QBFormatTest, testLoadRGB) {
	testRGB("rgb.qb");
}

TEST_F(QBFormatTest, testLoadRGBSmall) {
	testRGBSmall("rgb_small.qb");
}

TEST_F(QBFormatTest, testLoadRGBSmallSaveLoad) {
	testRGBSmallSaveLoad("rgb_small.qb");
}

TEST_F(QBFormatTest, testSaveSingleVoxel) {
	QBFormat f;
	testSaveSingleVoxel("qubicle-singlevoxelsavetest.qb", &f);
}

TEST_F(QBFormatTest, testSaveSmallVoxel) {
	QBFormat f;
	testSaveLoadVoxel("qubicle-smallvolumesavetest.qb", &f);
}

TEST_F(QBFormatTest, testSaveMultipleLayers) {
	QBFormat f;
	testSaveMultipleModels("qubicle-multiplemodelsavetest.qb", &f);
}

TEST_F(QBFormatTest, testLoadSave) {
	QBFormat f;
	testLoadSaveAndLoad("qubicle.qb", f, "qubicle-savetest.qb", f);
}

}
