#include <velk/api/hierarchy.h>
#include <velk/api/object.h>
#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>
#include <velk/plugins/data/api/data.h>
#include <velk-ui/interface/intf_list_row.h>
#include <velk-ui/interface/trait/intf_tree_view.h>
#include <velk-ui/plugins/text/intf_text_visual.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <utility>

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
        for (auto* name : {"plugin:velk_importer", "plugin:velk_render", "plugin:velk_scene", "plugin:velk_data",
                           "plugin:velk_ui", "plugin:velk_text"}) {
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
        source_ = create_hierarchy_source(hierarchy_);
        write_state<ITreeView>(tree_, [&](ITreeView::State& s) { set_object_ref(s.source, source_); });
        update();

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
        update();
    }

    // Rows created or moved during a layout settle on the next one.
    void update()
    {
        instance().update();
        instance().update();
    }

    /// The text of each row under the view, top to bottom, with its depth.
    vector<std::pair<string, uint32_t>> shown() const
    {
        vector<std::pair<uint32_t, std::pair<string, uint32_t>>> rows;
        auto* h = interface_cast<IHierarchy>(scene_.as_ptr<IScene>().get());
        for (auto& child : h->children_of(as_object(tree_element_.as_ptr<IElement>()))) {
            auto rs = read_state<ui::IListRow>(Element(interface_pointer_cast<IElement>(child)).find_attachment<ui::IListRow>());
            auto label = Element(interface_pointer_cast<IElement>(h->child_at(child, 0)));
            auto ts = read_state<ui::ITextVisual>(label.find_attachment<ui::ITextVisual>());
            rows.push_back({rs ? rs->index : 0u, {ts ? ts->text : string{}, rs ? rs->depth : 0u}});
        }
        std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first < b.first; });
        vector<std::pair<string, uint32_t>> out;
        for (auto& r : rows) {
            out.push_back(r.second);
        }
        return out;
    }

    void scroll(float lines)
    {
        ui::ScrollEvent ev;
        ev.position = {60.f, 30.f};
        ev.delta = {0.f, lines};
        ev.unit = ui::ScrollUnit::Lines;
        dispatcher_->scroll_event(ev);
        update();
    }

    size_t rows() const
    {
        auto state = read_state<IElement>(tree_element_.as_ptr<IElement>());
        return state ? static_cast<size_t>(state->size.height / 22.f + 0.5f) : 0;
    }

    IObject::Ptr selected() const { return read_state<ITreeView>(tree_)->selected.get<IObject>(); }

    Scene scene_;
    Hierarchy hierarchy_;
    IItemSource::Ptr source_;
    IObject::Ptr scene_node_, camera_, panel_, light_;
    Element tree_element_;
    ITreeView::Ptr tree_;
    IInputDispatcher::Ptr dispatcher_;
};

TEST_F(TreeViewTest, RootStartsExpanded)
{
    EXPECT_EQ(rows(), 4u); // Scene, Camera, Panel, Light
    auto visible = shown();
    ASSERT_EQ(visible.size(), 3u); // a 66px view shows three 22px rows
    EXPECT_EQ(visible[0], std::make_pair(string("Scene"), 0u));
    EXPECT_EQ(visible[1], std::make_pair(string("Camera"), 1u));
    EXPECT_EQ(visible[2], std::make_pair(string("Panel"), 1u));
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
    // Selecting keeps what is expanded.
    click(60.f, 5.f);
    EXPECT_EQ(rows(), 6u);
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
    update();
    EXPECT_EQ(rows(), 3u);
    hierarchy_.add(scene_node_, node("Fog"));
    update();
    EXPECT_EQ(rows(), 4u);
    auto visible = shown();
    ASSERT_EQ(visible.size(), 3u); // the view shows three of the four
    EXPECT_EQ(visible[1].first, string("Panel"));

    // A change under an expanded item updates only its rows.
    click(20.f, 22.f + 5.f); // expand Panel: Scene, Panel, Title, Body, Light, Fog
    EXPECT_EQ(rows(), 6u);
    hierarchy_.insert(panel_, 1, node("Middle"));
    update();
    EXPECT_EQ(rows(), 7u);
    scroll(-1.f); // 40px down: rows 1 to 4 show
    visible = shown();
    ASSERT_EQ(visible.size(), 4u);
    EXPECT_EQ(visible[0], std::make_pair(string("Panel"), 1u));
    EXPECT_EQ(visible[1], std::make_pair(string("Title"), 2u));
    EXPECT_EQ(visible[2], std::make_pair(string("Middle"), 2u));
    EXPECT_EQ(visible[3], std::make_pair(string("Body"), 2u));
}

TEST_F(TreeViewTest, ScrollsInTheView)
{
    click(20.f, 44.f + 5.f); // expand Panel: 6 rows, 132px in a 66px view
    scroll(-10.f);
    click(60.f, 66.f - 5.f); // the last row shows at the bottom of the view
    EXPECT_EQ(selected(), light_);
}

} // namespace
