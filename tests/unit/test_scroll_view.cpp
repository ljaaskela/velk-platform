#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/interface/trait/intf_hover.h>
#include <velk-ui/interface/trait/intf_scroll_view.h>
#include <velk-ui/plugin.h>

#include <gtest/gtest.h>

#include <cstring>

using namespace velk;
using ui::IInputDispatcher;
using ui::IScrollView;
using ui::PointerAction;
using ui::PointerButton;
using ui::PointerEvent;
using ui::ScrollEvent;

namespace {

// A 100x50 view over 80x200 content: a vertical bar 8px wide at x 92, nothing sideways.
class ScrollViewTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& reg = instance().plugin_registry();
        for (auto* name : {"plugin:velk_importer", "plugin:velk_render", "plugin:velk_scene", "plugin:velk_ui"}) {
            ASSERT_TRUE(succeeded(reg.load_plugin(string_view(name, std::strlen(name))))) << name;
        }
    }

    void SetUp() override
    {
        scene_ = create_scene(string("file://") + VELK_UI_TEST_FIXTURE "/scroll_view.velk");
        ASSERT_TRUE(scene_);
        scene_.set_geometry(aabb::from_size({400.f, 200.f}));
        instance().update();

        view_element_ = scene_.find_first<IScrollView>();
        ASSERT_TRUE(view_element_);
        view_ = view_element_.find_attachment<IScrollView>();
        content_ = scene_.find_first<ui::IHover>();
        ASSERT_TRUE(content_);

        dispatcher_ = instance().create<IInputDispatcher>(ui::ClassId::Input::Dispatcher);
        dispatcher_->set_scene(scene_.as_ptr<IScene>());
    }

    void wheel(float lines, bool shift = false)
    {
        ScrollEvent ev;
        ev.position = {40.f, 20.f};
        ev.delta = {0.f, lines};
        ev.unit = ui::ScrollUnit::Lines;
        ev.modifiers = shift ? ui::Modifier::Shift : ui::Modifier::None;
        dispatcher_->scroll_event(ev);
        instance().update();
    }

    void pointer(PointerAction action, float x, float y)
    {
        PointerEvent ev;
        ev.position = {x, y};
        ev.button = PointerButton::Left;
        ev.action = action;
        dispatcher_->pointer_event(ev);
        instance().update();
    }

    float scroll_y() const { return read_state<IScrollView>(view_)->scroll_y; }
    float scroll_x() const { return read_state<IScrollView>(view_)->scroll_x; }
    float content_y() const { return read_state<IElement>(content_.as_ptr<IElement>())->world_matrix(1, 3); }

    Scene scene_;
    Element view_element_;
    IScrollView::Ptr view_;
    Element content_;
    IInputDispatcher::Ptr dispatcher_;
};

TEST_F(ScrollViewTest, WheelScrollsTheContent)
{
    EXPECT_EQ(content_y(), 0.f);
    wheel(-1.f); // one line down: wheel_step, 40px
    EXPECT_EQ(scroll_y(), 40.f);
    EXPECT_EQ(content_y(), -40.f);
    wheel(1.f);
    EXPECT_EQ(scroll_y(), 0.f);
}

TEST_F(ScrollViewTest, ScrollStaysWithinTheContent)
{
    wheel(-10.f);
    EXPECT_EQ(scroll_y(), 150.f); // 200 of content, 50 shown
    wheel(10.f);
    EXPECT_EQ(scroll_y(), 0.f);
}

TEST_F(ScrollViewTest, NothingToScrollSideways)
{
    wheel(-1.f, true); // the content fits across
    EXPECT_EQ(scroll_x(), 0.f);
    EXPECT_EQ(scroll_y(), 0.f);
}

TEST_F(ScrollViewTest, DraggingTheThumb)
{
    // The thumb is 16px (the minimum; 50/200 of the track would be 12.5) at the top of the
    // 50px track, so 34px of travel covers 150px of content.
    pointer(PointerAction::Down, 95.f, 5.f);
    pointer(PointerAction::Move, 95.f, 22.f);
    EXPECT_FLOAT_EQ(scroll_y(), 17.f * 150.f / 34.f);
    pointer(PointerAction::Move, 95.f, 200.f);
    EXPECT_EQ(scroll_y(), 150.f);
    pointer(PointerAction::Up, 95.f, 200.f);
    pointer(PointerAction::Move, 95.f, 5.f); // released: moving no longer scrolls
    EXPECT_EQ(scroll_y(), 150.f);
}

TEST_F(ScrollViewTest, TrackClickPages)
{
    pointer(PointerAction::Down, 95.f, 45.f); // below the thumb
    pointer(PointerAction::Up, 95.f, 45.f);
    EXPECT_EQ(scroll_y(), 50.f);
}

TEST_F(ScrollViewTest, ClippedContentIsNotHit)
{
    pointer(PointerAction::Move, 40.f, 30.f); // inside the view
    EXPECT_EQ(dispatcher_->get_hovered(), content_.as_ptr<IElement>());
    pointer(PointerAction::Move, 40.f, 120.f); // the content is there, but clipped away
    EXPECT_NE(dispatcher_->get_hovered(), content_.as_ptr<IElement>());
}

} // namespace
