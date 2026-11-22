#include <velk/api/event.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>

#include <velk-scene/api/element.h>
#include <velk-scene/api/scene.h>
#include <velk-ui/interface/intf_input_dispatcher.h>
#include <velk-ui/plugin.h>
#include <velk-ui/plugins/text/intf_text_field.h>
#include <velk-ui/plugins/text/intf_text_visual.h>

#include <gtest/gtest.h>

#include <cstring>

using namespace velk;
using ui::IInputDispatcher;
using ui::ITextField;
using ui::ITextVisual;
using ui::Key;
using ui::KeyAction;
using ui::KeyEvent;
using ui::PointerAction;
using ui::PointerButton;
using ui::PointerEvent;

namespace {

class TextFieldTest : public ::testing::Test
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
        scene_ = create_scene(string("file://") + VELK_UI_TEST_FIXTURE "/text_field.velk");
        ASSERT_TRUE(scene_);
        scene_.set_geometry(aabb::from_size({400.f, 200.f}));
        instance().update(); // runs the layout solver, then the scene update

        field_element_ = scene_.find_first<ITextField>();
        ASSERT_TRUE(field_element_);
        field_ = field_element_.find_attachment<ITextField>();
        ASSERT_TRUE(field_);

        dispatcher_ = instance().create<IInputDispatcher>(ui::ClassId::Input::Dispatcher);
        ASSERT_TRUE(dispatcher_);
        dispatcher_->set_scene(scene_.as_ptr<IScene>());

        commit_sub_ = ScopedHandler(field_->on_commit(), [this](const string& text) { commits_.push_back(text); });
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
    }

    void type(string_view text)
    {
        for (char c : text) {
            KeyEvent ev;
            ev.action = KeyAction::Text;
            ev.codepoint = static_cast<unsigned char>(c);
            dispatcher_->key_event(ev);
        }
    }

    void type_codepoint(char32_t cp)
    {
        KeyEvent ev;
        ev.action = KeyAction::Text;
        ev.codepoint = cp;
        dispatcher_->key_event(ev);
    }

    void press(Key key)
    {
        KeyEvent ev;
        ev.named = key;
        ev.action = KeyAction::Down;
        dispatcher_->key_event(ev);
        ev.action = KeyAction::Up;
        dispatcher_->key_event(ev);
    }

    string text() const
    {
        auto state = read_state<ITextVisual>(field_element_.find_attachment<ITextVisual>());
        return state ? state->text : string();
    }

    bool editing() const
    {
        auto state = read_state<ITextField>(field_);
        return state && state->editing;
    }

    Scene scene_;
    Element field_element_;
    ITextField::Ptr field_;
    IInputDispatcher::Ptr dispatcher_;
    vector<string> commits_;
    ScopedHandler commit_sub_;
};

TEST_F(TextFieldTest, ClickFocusesAndEnterCommits)
{
    EXPECT_FALSE(editing());
    click(190.f, 15.f);
    EXPECT_TRUE(editing());
    EXPECT_EQ(dispatcher_->get_focused(), field_element_.as_ptr<IElement>());

    type("c");
    EXPECT_EQ(text(), "ab"); // text keeps its value while editing
    press(Key::Enter);
    EXPECT_FALSE(editing());
    EXPECT_EQ(text(), "abc");
    ASSERT_EQ(commits_.size(), 1u);
    EXPECT_EQ(commits_[0], "abc");
}

TEST_F(TextFieldTest, EditingKeys)
{
    click(190.f, 15.f); // caret at the end: "ab|"
    press(Key::Backspace); // "a|"
    press(Key::Home);      // "|a"
    type("x");             // "x|a"
    press(Key::End);       // "xa|"
    type("y");             // "xay|"
    press(Key::Delete);    // nothing after the caret
    press(Key::Left);      // "xa|y"
    press(Key::Delete);    // "xa|"
    press(Key::Left);      // "x|a"
    press(Key::Right);     // "xa|"
    type("z");             // "xaz|"
    press(Key::Enter);
    EXPECT_EQ(text(), "xaz");
}

TEST_F(TextFieldTest, EscapeCancels)
{
    click(190.f, 15.f);
    type("zz");
    press(Key::Escape);
    EXPECT_FALSE(editing());
    EXPECT_EQ(text(), "ab");
    EXPECT_TRUE(commits_.empty());
}

TEST_F(TextFieldTest, ClickElsewhereCommits)
{
    click(190.f, 15.f);
    type("q");
    click(300.f, 150.f);
    EXPECT_FALSE(editing());
    EXPECT_FALSE(dispatcher_->get_focused());
    EXPECT_EQ(text(), "abq");
    EXPECT_EQ(commits_.size(), 1u);
}

TEST_F(TextFieldTest, ClickPlacesCaret)
{
    click(0.f, 15.f); // before the first character
    type("x");
    press(Key::Enter);
    EXPECT_EQ(text(), "xab");
}

TEST_F(TextFieldTest, UnchangedTextDoesNotCommit)
{
    click(190.f, 15.f);
    type("q");
    press(Key::Backspace);
    press(Key::Enter);
    EXPECT_EQ(text(), "ab");
    EXPECT_TRUE(commits_.empty());
}

TEST_F(TextFieldTest, BackspaceRemovesAWholeCharacter)
{
    click(190.f, 15.f);
    type_codepoint(char32_t(0xE4));   // a umlaut: two bytes in UTF-8
    type_codepoint(char32_t(0x20AC)); // euro sign: three bytes
    press(Key::Backspace);
    press(Key::Enter);
    EXPECT_EQ(text(), "ab\xc3\xa4");
}

TEST_F(TextFieldTest, KeysWithoutFocusAreIgnored)
{
    type("q");
    press(Key::Backspace);
    EXPECT_EQ(text(), "ab");
    EXPECT_FALSE(editing());
}

TEST_F(TextFieldTest, ClickAfterEnterEditsAgain)
{
    click(190.f, 15.f);
    type("1");
    press(Key::Enter);
    click(190.f, 15.f); // focus has not moved; the click starts a new edit
    EXPECT_TRUE(editing());
    type("2");
    press(Key::Enter);
    EXPECT_EQ(text(), "ab12");
}

TEST_F(TextFieldTest, TypingDoesNotReachGlobalKeyHandlers)
{
    int global = 0;
    ScopedHandler sub(dispatcher_->on_key_event(), [&global](const KeyEvent&) { ++global; });

    KeyEvent w;
    w.key = 87; // a character key: no named key
    w.action = KeyAction::Down;

    dispatcher_->key_event(w); // nothing focused: the broadcast gets it
    EXPECT_EQ(global, 1);

    click(190.f, 15.f);
    dispatcher_->key_event(w); // the field takes it
    type("w");
    KeyEvent left;
    left.named = Key::Left;
    left.action = KeyAction::Down;
    dispatcher_->key_event(left);
    EXPECT_EQ(global, 1);

    w.action = KeyAction::Up; // releases go on
    dispatcher_->key_event(w);
    EXPECT_EQ(global, 2);

    w.action = KeyAction::Down; // shortcuts go on
    w.modifiers = ui::Modifier::Ctrl;
    dispatcher_->key_event(w);
    EXPECT_EQ(global, 3);

    press(Key::Enter); // ends editing (its release reaches the broadcast)
    global = 0;
    w.modifiers = ui::Modifier::None;
    dispatcher_->key_event(w);
    EXPECT_EQ(global, 1);
}

} // namespace
