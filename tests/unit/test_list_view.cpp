#include <velk/api/object.h>
#include <velk/api/property.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/plugins/data/api/data.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/interface/intf_list_row.h>
#include <velk-ui/interface/trait/intf_list_view.h>
#include <velk-ui/interface/trait/intf_scroll_view.h>
#include <velk-ui/plugin.h>
#include <velk-ui/plugins/text/intf_text_visual.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <algorithm>
#include <cstring>

using namespace velk;
using ui::IInputDispatcher;
using ui::IListRow;
using ui::IListView;
using ui::PointerAction;
using ui::PointerButton;
using ui::PointerEvent;

namespace {

IObject::Ptr item(int n)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Item %d", n);
    auto object = create_object().as_ptr<IObject>();
    auto* meta = interface_cast<IMetadata>(object);
    Property<string>(meta->add_property("label", type_uid<string>(), nullptr)).set_value(string(buf));
    return object;
}

// A 200x100 ScrollView over a list of 10,000 items in 20px rows: five rows show.
class ListViewTest : public ::testing::Test
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
        scene_ = create_scene(string("file://") + VELK_UI_TEST_FIXTURE "/list_view.velk");
        ASSERT_TRUE(scene_);
        scene_.set_geometry(aabb::from_size({400.f, 200.f}));

        list_ = create_object_list();
        ASSERT_TRUE(list_);
        vector<IObject::Ptr> items;
        for (int i = 0; i < 10000; ++i) {
            items.push_back(item(i));
        }
        list_->insert(0, {items.data(), items.size()});

        list_element_ = scene_.find_first<IListView>();
        ASSERT_TRUE(list_element_);
        view_ = list_element_.find_attachment<IListView>();
        write_state<IListView>(view_, [&](IListView::State& s) {
            set_object_ref(s.source, list_);
            s.row_template = string("file://") + VELK_UI_TEST_FIXTURE "/list_row.velk";
        });
        scroll_ = scene_.find_first<ui::IScrollView>().find_attachment<ui::IScrollView>();
        update();

        dispatcher_ = instance().create<IInputDispatcher>(ui::ClassId::Input::Dispatcher);
        dispatcher_->set_scene(scene_.as_ptr<IScene>());
    }

    // Rows created or moved during a layout settle on the next one.
    void update()
    {
        instance().update();
        instance().update();
    }

    void scroll_to(float y)
    {
        write_state<ui::IScrollView>(scroll_, [&](ui::IScrollView::State& s) { s.scroll_y = y; });
        update();
    }

    struct Shown
    {
        uint32_t index;
        string text;
        float y;
    };

    /// The rows under the list, sorted by index.
    vector<Shown> rows() const
    {
        vector<Shown> out;
        auto* h = interface_cast<IHierarchy>(scene_.as_ptr<IScene>().get());
        for (auto& child : h->children_of(as_object(list_element_.as_ptr<IElement>()))) {
            Element row(interface_pointer_cast<IElement>(child));
            auto rs = read_state<IListRow>(row.find_attachment<IListRow>());
            auto label = Element(interface_pointer_cast<IElement>(h->child_at(child, 0)));
            auto ts = read_state<ui::ITextVisual>(label.find_attachment<ui::ITextVisual>());
            auto es = read_state<IElement>(row.as_ptr<IElement>());
            out.push_back({rs ? rs->index : 0u, ts ? ts->text : string{}, es ? es->world_matrix(1, 3) : -1.f});
        }
        std::sort(out.begin(), out.end(), [](const Shown& a, const Shown& b) { return a.index < b.index; });
        return out;
    }

    float content_height() const { return read_state<IElement>(list_element_.as_ptr<IElement>())->size.height; }

    Scene scene_;
    IObjectList::Ptr list_;
    Element list_element_;
    IListView::Ptr view_;
    ui::IScrollView::Ptr scroll_;
    IInputDispatcher::Ptr dispatcher_;
};

TEST_F(ListViewTest, OnlyVisibleRowsExist)
{
    EXPECT_FLOAT_EQ(content_height(), 200000.f);
    auto shown = rows();
    ASSERT_EQ(shown.size(), 5u);
    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(shown[i].index, i);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Item %u", i);
        EXPECT_EQ(shown[i].text, string(buf));
        EXPECT_FLOAT_EQ(shown[i].y, 20.f * static_cast<float>(i));
    }
}

TEST_F(ListViewTest, ScrollReusesRows)
{
    scroll_to(1010.f);
    auto shown = rows();
    // 1010 to 1110 crosses rows 50 to 55.
    ASSERT_EQ(shown.size(), 6u);
    EXPECT_EQ(shown.front().index, 50u);
    EXPECT_EQ(shown.front().text, string("Item 50"));
    EXPECT_FLOAT_EQ(shown.front().y, -10.f);
    EXPECT_EQ(shown.back().index, 55u);
    EXPECT_EQ(shown.back().text, string("Item 55"));
}

TEST_F(ListViewTest, InsertAndRemoveUpdateTheRows)
{
    const IObject::Ptr one[] = {item(-1)};
    list_->insert(0, one);
    update();
    auto shown = rows();
    ASSERT_EQ(shown.size(), 5u);
    EXPECT_EQ(shown[0].text, string("Item -1"));
    EXPECT_EQ(shown[1].text, string("Item 0"));
    EXPECT_FLOAT_EQ(content_height(), 200020.f);

    list_->remove(0, 3);
    update();
    shown = rows();
    ASSERT_EQ(shown.size(), 5u);
    EXPECT_EQ(shown[0].text, string("Item 2"));
    EXPECT_EQ(shown[4].text, string("Item 6"));
}

TEST_F(ListViewTest, ClickSelects)
{
    PointerEvent ev;
    ev.position = {40.f, 50.f}; // the third row
    ev.button = PointerButton::Left;
    ev.action = PointerAction::Down;
    dispatcher_->pointer_event(ev);
    ev.action = PointerAction::Up;
    dispatcher_->pointer_event(ev);
    update();

    auto vs = read_state<IListView>(view_);
    ASSERT_TRUE(vs);
    EXPECT_EQ(vs->selected.get(), list_->at(2));
    Element row;
    auto* h = interface_cast<IHierarchy>(scene_.as_ptr<IScene>().get());
    int selected_rows = 0;
    for (auto& child : h->children_of(as_object(list_element_.as_ptr<IElement>()))) {
        auto rs = read_state<IListRow>(Element(interface_pointer_cast<IElement>(child)).find_attachment<IListRow>());
        if (rs && rs->selected) {
            ++selected_rows;
            EXPECT_EQ(rs->index, 2u);
        }
    }
    EXPECT_EQ(selected_rows, 1);
}

} // namespace
