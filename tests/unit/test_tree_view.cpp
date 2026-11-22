#include <velk/api/hierarchy.h>
#include <velk/api/object.h>
#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>
#include <velk-ui/plugins/text/intf_tree_view.h>

#include <gtest/gtest.h>

#include <cstring>

using namespace velk;
using ui::IInputDispatcher;
using ui::ITreeView;
using ui::PointerAction;
using ui::PointerButton;
using ui::PointerEvent;

namespace {

IObject::Ptr node(string_view label)
{
    auto object = create_object().as_ptr<IObject>();
    auto* meta = interface_cast<IMetadata>(object);
    Property<string>(meta->add_property("label", type_uid<string>(), nullptr)).set_value(string(label));
    return object;
}

// Rows are 22px; the view shows three of them.
class TreeViewTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& reg = instance().plugin_registry();
        for (auto* name : {"plugin:velk_importer", "plugin:velk_render", "plugin:velk_scene", "plugin:velk_ui",
                           "plugin:velk_text"}) {
            ASSERT_TRUE(succeeded(reg.load_plugin(string_view(name, std::strlen(name))))) << name;
        }
    }

    void SetUp() override
    {
        scene_ = create_scene(string("file://") + VELK_UI_TEST_FIXTURE "/tree_view.velk");
        ASSERT_TRUE(scene_);
        scene_.set_geometry(aabb::from_size({400.f, 200.f}));

        // Scene: Camera, Panel (Title, Body), Light
        hierarchy_ = create_hierarchy();
        scene_node_ = node("Scene");
        camera_ = node("Camera");
        panel_ = node("Panel");
        light_ = node("Light");
        hierarchy_.set_root(scene_node_);
        hierarchy_.add(scene_node_, camera_);
        hierarchy_.add(scene_node_, panel_);
        hierarchy_.add(scene_node_, light_);
        hierarchy_.add(panel_, node("Title"));
        hierarchy_.add(panel_, node("Body"));

        tree_element_ = scene_.find_first<ITreeView>();
        ASSERT_TRUE(tree_element_);
        tree_ = tree_element_.find_attachment<ITreeView>();
        write_state<ITreeView>(tree_, [&](ITreeView::State& s) { set_object_ref(s.source, hierarchy_.as_ptr<IObject>()); });
        instance().update();

        dispatcher_ = instance().create<IInputDispatcher>(ui::ClassId::Input::Dispatcher);
        dispatcher_->set_scene(scene_.as_ptr<IScene>());
    }

    void click(float x, float y)
    {
        PointerEvent ev;
        ev.position = {x, y};
        ev.button = PointerButton::Left;
        ev.action = PointerAction::Down;
        dispatcher_->pointer_event(ev);
        ev.action = PointerAction::Up;
        dispatcher_->pointer_event(ev);
        instance().update();
    }

    size_t rows() const
    {
        auto state = read_state<IElement>(tree_element_.as_ptr<IElement>());
        return state ? static_cast<size_t>(state->size.height / 22.f + 0.5f) : 0;
    }

    IObject::Ptr selected() const { return read_state<ITreeView>(tree_)->selected.get<IObject>(); }

    Scene scene_;
    Hierarchy hierarchy_;
    IObject::Ptr scene_node_, camera_, panel_, light_;
    Element tree_element_;
    ITreeView::Ptr tree_;
    IInputDispatcher::Ptr dispatcher_;
};

TEST_F(TreeViewTest, RootStartsExpanded)
{
    EXPECT_EQ(rows(), 4u); // Scene, Camera, Panel, Light
}

TEST_F(TreeViewTest, ClickSelects)
{
    click(60.f, 22.f + 5.f); // Camera
    EXPECT_EQ(selected(), camera_);
    click(60.f, 5.f); // Scene
    EXPECT_EQ(selected(), scene_node_);
}

TEST_F(TreeViewTest, MarkerExpandsAndCollapses)
{
    click(20.f, 44.f + 5.f); // the + of Panel, one level in
    EXPECT_EQ(rows(), 6u);
    EXPECT_FALSE(selected());
    click(20.f, 44.f + 5.f);
    EXPECT_EQ(rows(), 4u);
}

TEST_F(TreeViewTest, CollapsingTheRoot)
{
    click(4.f, 5.f);
    EXPECT_EQ(rows(), 1u);
}

TEST_F(TreeViewTest, FollowsHierarchyChanges)
{
    hierarchy_.remove(camera_);
    instance().update();
    EXPECT_EQ(rows(), 3u);
    hierarchy_.add(scene_node_, node("Fog"));
    instance().update();
    EXPECT_EQ(rows(), 4u);
}

TEST_F(TreeViewTest, ScrollsInTheView)
{
    click(20.f, 44.f + 5.f); // expand Panel: 6 rows, 132px in a 66px view
    ui::ScrollEvent ev;
    ev.position = {60.f, 30.f};
    ev.delta = {0.f, -10.f};
    ev.unit = ui::ScrollUnit::Lines;
    dispatcher_->scroll_event(ev);
    instance().update();
    click(60.f, 66.f - 5.f); // the last row shows at the bottom of the view
    EXPECT_EQ(selected(), light_);
}

} // namespace
