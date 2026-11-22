#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_scroll_view.h>
#include <velk-ui/plugin.h>

#include <gtest/gtest.h>

#include <cstring>

using namespace velk;

namespace {

/// Content 80x200 that records what layout gives it.
class Probe : public ui::ext::LayoutTrait<Probe, TraitPhase::Layout>
{
public:
    VELK_CLASS_UID("4f3b8f2e-6a51-4d0e-9a57-1c2f3d4e5a61", "VisibleProbe");

    TraitPhase get_phase() const override { return TraitPhase::Constraint | TraitPhase::Layout; }

    void describe(ui::LayoutBox& box) const override
    {
        box.width = dim::auto_size();
        box.height = dim::auto_size();
    }
    size measure(const size&, IElement&, ui::ILayoutContext&) override
    {
        ++measures;
        return {80.f, 200.f, 0.f};
    }
    void arrange(const aabb&, IElement&, ui::ILayoutContext& ctx) override { visible = ctx.visible(); }

    int measures = 0;
    rect visible{};
};

// A 100x50 ScrollView over the probe: an 8px vertical bar, so the viewport is 92x50.
class LayoutVisibleTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& reg = instance().plugin_registry();
        for (auto* name : {"plugin:velk_importer", "plugin:velk_render", "plugin:velk_scene", "plugin:velk_ui"}) {
            ASSERT_TRUE(succeeded(reg.load_plugin(string_view(name, std::strlen(name))))) << name;
        }
        instance().type_registry().register_type<Probe>();
    }
    static void TearDownTestSuite() { instance().type_registry().unregister_type<Probe>(); }

    void SetUp() override
    {
        scene_ = create_scene(string("file://") + VELK_UI_TEST_FIXTURE "/visible.velk");
        ASSERT_TRUE(scene_);
        scene_.set_geometry(aabb::from_size({400.f, 200.f}));
        instance().update();

        auto view_element = scene_.find_first<ui::IScrollView>();
        ASSERT_TRUE(view_element);
        view_ = view_element.find_attachment<ui::IScrollView>();
        auto* h = interface_cast<IHierarchy>(scene_.as_ptr<IScene>().get());
        auto content = interface_pointer_cast<IElement>(h->child_at(as_object(view_element.as_ptr<IElement>()), 0));
        ASSERT_TRUE(content);
        auto probe = instance().create<ui::ILayoutTrait>(Probe::class_uid);
        ASSERT_TRUE(probe);
        probe_ = static_cast<Probe*>(probe.get());
        probe_ptr_ = probe;
        Element(content).add_attachment(probe);
        instance().update();
    }

    void scroll_to(float y)
    {
        write_state<ui::IScrollView>(view_, [&](ui::IScrollView::State& s) { s.scroll_y = y; });
        instance().update();
    }

    Scene scene_;
    ui::IScrollView::Ptr view_;
    ui::ILayoutTrait::Ptr probe_ptr_;
    Probe* probe_ = nullptr;
};

TEST_F(LayoutVisibleTest, ContentSeesTheViewport)
{
    EXPECT_EQ(probe_->visible, (rect{0.f, 0.f, 80.f, 50.f}));
}

TEST_F(LayoutVisibleTest, ScrollMovesTheVisibleRectWithoutMeasuring)
{
    const int measures = probe_->measures;
    scroll_to(40.f);
    EXPECT_EQ(probe_->visible, (rect{0.f, 40.f, 80.f, 50.f}));
    scroll_to(150.f);
    EXPECT_EQ(probe_->visible, (rect{0.f, 150.f, 80.f, 50.f}));
    EXPECT_EQ(probe_->measures, measures);
}

} // namespace
