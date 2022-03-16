/**
 * @file
 */

#include "app/tests/AbstractTest.h"
#include "voxel/tests/TestHelper.h"
#include "voxelformat/SceneGraph.h"
#include "voxelformat/SceneGraphNode.h"

namespace voxelformat {

class SceneGraphTest: public app::AbstractTest {
};

TEST_F(SceneGraphTest, testSize) {
	SceneGraph sceneGraph;
	EXPECT_EQ(1u, sceneGraph.size(SceneGraphNodeType::Root)) << "Each scene graph should contain a root node by default";
	EXPECT_TRUE(sceneGraph.empty()) << "There are no model nodes yet - thus empty should return true";
	{
		SceneGraphNode node;
		node.setName("node1");
		sceneGraph.emplace(core::move(node));
	}
	{
		SceneGraphNode node;
		node.setName("node2");
		sceneGraph.emplace(core::move(node));
	}
	EXPECT_EQ(2u, sceneGraph.size(SceneGraphNodeType::Model)) << "The scene graph should have two models";
	EXPECT_EQ(2u, sceneGraph.size()) << "The scene graph should have two models";

	EXPECT_EQ(2u, sceneGraph.root().children().size()) << "The root node should have two (model) children attached";
}

TEST_F(SceneGraphTest, testHasNode) {
	SceneGraph sceneGraph;
	EXPECT_TRUE(sceneGraph.hasNode(0));
	EXPECT_FALSE(sceneGraph.hasNode(1));
	SceneGraphNode node;
	node.setName("node");
	EXPECT_EQ(1, sceneGraph.emplace(core::move(node)));
	EXPECT_TRUE(sceneGraph.hasNode(0));
	EXPECT_TRUE(sceneGraph.hasNode(1));
	EXPECT_FALSE(sceneGraph.hasNode(2));
}

TEST_F(SceneGraphTest, testNodeRoot) {
	SceneGraph sceneGraph;
	const SceneGraphNode& root = sceneGraph.node(0);
	EXPECT_EQ(0, root.id());
	EXPECT_EQ(SceneGraphNodeType::Root, root.type());
}

TEST_F(SceneGraphTest, testNode) {
	SceneGraph sceneGraph;
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("node");
		sceneGraph.emplace(core::move(node));
	}
	const SceneGraphNode& modelNode = sceneGraph.node(1);
	EXPECT_EQ(SceneGraphNodeType::Model, modelNode.type());
	EXPECT_EQ(1, modelNode.id());
	EXPECT_EQ("node", modelNode.name());
}

TEST_F(SceneGraphTest, testChildren) {
	SceneGraph sceneGraph;
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("model");
		EXPECT_EQ(1, sceneGraph.emplace(core::move(node), 0)) << "Unexpected node id returned - root node is 0 - next should be 1";
	}
	{
		SceneGraphNode node(SceneGraphNodeType::Group);
		node.setName("group");
		EXPECT_EQ(2, sceneGraph.emplace(core::move(node), 1));
	}
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("model2");
		EXPECT_EQ(3, sceneGraph.emplace(core::move(node), 2));
	}
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("model");
		EXPECT_EQ(4, sceneGraph.emplace(core::move(node), 1));
	}
	EXPECT_EQ(1, sceneGraph.root().children()[0]);
	ASSERT_TRUE(sceneGraph.hasNode(1));
	const SceneGraphNode& modelNode = sceneGraph.node(1);
	EXPECT_EQ(SceneGraphNodeType::Model, modelNode.type());
	EXPECT_EQ(1, modelNode.id());
	EXPECT_EQ("model", modelNode.name());
	ASSERT_EQ(2u, modelNode.children().size());
	EXPECT_EQ(2, modelNode.children()[0]) << "First child should be the node with the id 2";
	ASSERT_TRUE(sceneGraph.hasNode(2));
	EXPECT_EQ(modelNode.id(), sceneGraph.node(2).parent());
	EXPECT_EQ(4, modelNode.children()[1]) << "Second child should be the node with the id 4";
	ASSERT_TRUE(sceneGraph.hasNode(4));
	EXPECT_EQ(modelNode.id(), sceneGraph.node(4).parent());
	EXPECT_EQ(3u, sceneGraph.size(SceneGraphNodeType::Model));
	ASSERT_EQ(1u, sceneGraph.root().children().size());
}

TEST_F(SceneGraphTest, testRemove) {
	SceneGraph sceneGraph;
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("node");
		sceneGraph.emplace(core::move(node));
	}
	{
		SceneGraphNode node(SceneGraphNodeType::Model);
		node.setName("children");
		sceneGraph.emplace(core::move(node), 1);
	}
	EXPECT_EQ(2u, sceneGraph.size(SceneGraphNodeType::Model));
	EXPECT_TRUE(sceneGraph.removeNode(1, true));
	EXPECT_EQ(0u, sceneGraph.size(SceneGraphNodeType::Model));
	EXPECT_TRUE(sceneGraph.empty(SceneGraphNodeType::Model));
}

}
