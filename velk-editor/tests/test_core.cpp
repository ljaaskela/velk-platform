// Launches a copy of the fixture app with velk_run and edits it through the core: apply, undo,
// redo, reset, save.

#include <velk/api/future.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/json.h>
#include <velk/plugins/remote/api/remote.h>

#include <velk-editor/core/intf_editor_core.h>
#include <velk-editor/core/plugin.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace velk;
using editor::IEditorCore;
using editor::IEditorNode;

namespace {

/// Runs updates until @p done, or @p seconds pass.
template <class F>
bool pump_until(F done, int seconds = 20)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done() && std::chrono::steady_clock::now() < until) {
        instance().update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

template <class T>
T resolve(const IFuture::Ptr& f, T fallback = {})
{
    Future<T> future(f);
    if (!pump_until([&] { return future.is_ready(); })) {
        return fallback;
    }
    auto r = future.get_result();
    return r ? r.get_value() : fallback;
}

namespace fs = std::filesystem;

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        out.push_back(line);
    }
    return out;
}

JsonValue parse(const string& text)
{
    JsonValue v;
    string error;
    EXPECT_TRUE(json_parse(text, v, error)) << error.c_str() << ": " << text.c_str();
    return v;
}

class CoreTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto& plugins = instance().plugin_registry();
        load_remote_plugin();
        plugins.load_plugin("plugin:velk_importer");
        // velk_ui's type extensions convert its values ("200px").
        plugins.load_plugin("plugin:velk_render");
        plugins.load_plugin("plugin:velk_scene");
        plugins.load_plugin("plugin:velk_ui");
        plugins.load_plugin("plugin:velk_editor_core");
    }

    void SetUp() override
    {
        // Each test edits its own copy; the checked-in fixture is never written.
        static std::atomic<int> counter{0};
        folder_ = fs::temp_directory_path()
                  / ("velk_editor_test_" + std::to_string(static_cast<unsigned long>(std::time(nullptr))) + "_"
                     + std::to_string(++counter));
        fs::remove_all(folder_);
        fs::create_directories(folder_);
        fs::copy(VELK_EDITOR_TEST_FIXTURE, folder_, fs::copy_options::recursive);
        core_ = instance().create<IEditorCore>(editor::ClassId::EditorCore);
        ASSERT_TRUE(core_);
        launch();
    }

    void TearDown() override
    {
        if (core_) {
            core_->fn_detach();
        }
        core_ = nullptr;
        std::error_code ec;
        fs::remove_all(folder_, ec);
    }

    void launch()
    {
        const std::string folder = folder_.u8string();
        ASSERT_TRUE(succeeded(core_->fn_launch(string(string_view(folder.data(), folder.size())))));
        const uint32_t revision = state()->revision;
        ASSERT_TRUE(pump_until([&] { return state()->revision != revision; }));
        ASSERT_TRUE(state()->editing);
    }

    std::string scene_text() { return read_file(folder_ / "scene.velk"); }

    /// Sets the width of @p object.
    string set_width(uint64_t object, const char* width)
    {
        char commands[160];
        std::snprintf(commands, sizeof(commands), R"([{"op":"set","object":%llu,"name":"width","value":"%s"}])",
                      static_cast<unsigned long long>(object), width);
        return apply(commands);
    }

    fs::path folder_;

    detail::StateReader<IEditorCore> state() { return read_state<IEditorCore>(core_); }

    JsonValue describe(uint64_t object) { return parse(resolve<string>(core_->fn_describe(object))); }

    string apply(const char* commands)
    {
        return resolve<string>(core_->fn_apply(string(string_view(commands, std::strlen(commands)))));
    }

    /// The attachment with @p entry, found through the tree's objects.
    uint64_t attachment(string_view entry)
    {
        auto tree = state()->tree.get<IHierarchy>();
        vector<IObject::Ptr> pending{tree->root()};
        while (!pending.empty()) {
            auto node = pending.back();
            pending.pop_back();
            for (auto& child : tree->children_of(node)) {
                pending.push_back(child);
            }
            const uint64_t object = read_state<IEditorNode>(node)->object;
            if (!object) {
                continue;
            }
            const JsonValue d = describe(object);
            for (auto& a : d["attachments"].as_array()) {
                const auto handle = static_cast<uint64_t>(a["object"].as_number());
                if (string_view(describe(handle)["entry"].as_string()) == entry) {
                    return handle;
                }
            }
        }
        return 0;
    }

    IEditorCore::Ptr core_;
};

} // namespace

TEST_F(CoreTest, DescribeShowsSourceAndAuthoredValues)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    auto d = describe(size);
    EXPECT_NE(d["source"].as_string().find("scene.velk"), string::npos);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("200px"));
    EXPECT_EQ(d["properties"]["width"].as_string(), string("200px"));
}

TEST_F(CoreTest, ApplyUndoRedo)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    char commands[256];
    std::snprintf(commands, sizeof(commands),
                  R"([{"op":"set","object":%llu,"name":"width","value":"120px"},
                      {"op":"set","object":%llu,"name":"height","value":"30px"}])",
                  static_cast<unsigned long long>(size), static_cast<unsigned long long>(size));
    EXPECT_EQ(apply(commands), string("{\"ok\":true}"));
    EXPECT_TRUE(state()->can_undo);

    auto d = describe(size);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("120px"));
    EXPECT_EQ(d["authored"]["height"].as_string(), string("30px"));
    EXPECT_EQ(d["properties"]["width"].as_string(), string("120px"));
    EXPECT_EQ(d["properties"]["height"].as_string(), string("30px"));

    EXPECT_TRUE(succeeded(resolve<ReturnValue>(core_->fn_undo(), ReturnValue::Fail)));
    d = describe(size);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("200px"));
    EXPECT_EQ(d["authored"].find("height"), nullptr);
    EXPECT_EQ(d["properties"]["width"].as_string(), string("200px"));
    EXPECT_FALSE(state()->can_undo);
    EXPECT_TRUE(state()->can_redo);

    EXPECT_TRUE(succeeded(resolve<ReturnValue>(core_->fn_redo(), ReturnValue::Fail)));
    d = describe(size);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("120px"));
    EXPECT_EQ(d["properties"]["height"].as_string(), string("30px"));
}

TEST_F(CoreTest, ResetRemovesTheValue)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    char commands[128];
    std::snprintf(commands, sizeof(commands), R"([{"op":"reset","object":%llu,"name":"width"}])",
                  static_cast<unsigned long long>(size));
    EXPECT_EQ(apply(commands), string("{\"ok\":true}"));
    auto d = describe(size);
    EXPECT_EQ(d["authored"].find("width"), nullptr);
    EXPECT_NE(d["properties"]["width"].as_string(), string("200px"));

    EXPECT_TRUE(succeeded(resolve<ReturnValue>(core_->fn_undo(), ReturnValue::Fail)));
    d = describe(size);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("200px"));
    EXPECT_EQ(d["properties"]["width"].as_string(), string("200px"));
}

TEST_F(CoreTest, RefusesBadCommands)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    char commands[128];
    std::snprintf(commands, sizeof(commands), R"([{"op":"set","object":%llu,"name":"width","value":true}])",
                  static_cast<unsigned long long>(size));
    EXPECT_NE(apply(commands).find("\"error\""), string::npos);
    EXPECT_NE(apply(R"([{"op":"move"}])").find("\"error\""), string::npos);
    EXPECT_NE(apply("not json").find("\"error\""), string::npos);
    EXPECT_FALSE(state()->can_undo);
    EXPECT_EQ(describe(size)["authored"]["width"].as_string(), string("200px"));
}

TEST_F(CoreTest, SaveWritesTheFile)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    const std::string before = scene_text();
    EXPECT_FALSE(state()->modified);
    EXPECT_EQ(set_width(size, "120px"), string("{\"ok\":true}"));
    EXPECT_TRUE(state()->modified);
    EXPECT_EQ(scene_text(), before); // nothing written until save

    const string saved = core_->fn_save();
    EXPECT_NE(saved.find("scene.velk"), string::npos) << saved.c_str();
    EXPECT_FALSE(state()->modified);
    EXPECT_FALSE(fs::exists(folder_ / "scene.velk.tmp"));

    // The file is the original with the one value changed.
    JsonValue expected = parse(string(string_view(before.data(), before.size())));
    for (auto& a : expected.find("attachments")->as_array()) {
        if (a["id"].as_string() == string("panel_size")) {
            a.find("properties")->replace("width", JsonValue::string("120px"));
        }
    }
    const std::string after = scene_text();
    EXPECT_TRUE(parse(string(string_view(after.data(), after.size()))) == expected) << after;
    EXPECT_EQ(string(string_view(after.data(), after.size())), json_write(expected));

    EXPECT_EQ(core_->fn_save(), string("{\"saved\":[]}"));
}

TEST_F(CoreTest, SecondSaveChangesOneLine)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    EXPECT_EQ(set_width(size, "120px"), string("{\"ok\":true}"));
    core_->fn_save();
    const auto first = lines_of(scene_text());
    EXPECT_EQ(set_width(size, "90px"), string("{\"ok\":true}"));
    core_->fn_save();
    const auto second = lines_of(scene_text());
    ASSERT_EQ(first.size(), second.size());
    size_t changed = 0;
    for (size_t i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) {
            ++changed;
            EXPECT_NE(second[i].find("90px"), std::string::npos) << second[i];
        }
    }
    EXPECT_EQ(changed, 1u);
}

TEST_F(CoreTest, SavedFileLoadsInANewLaunch)
{
    const uint64_t size = attachment("attachments/panel_size");
    ASSERT_NE(size, 0u);
    EXPECT_EQ(set_width(size, "120px"), string("{\"ok\":true}"));
    core_->fn_save();
    core_->fn_detach();
    EXPECT_FALSE(state()->editing);
    launch();
    const uint64_t reloaded = attachment("attachments/panel_size");
    ASSERT_NE(reloaded, 0u);
    auto d = describe(reloaded);
    EXPECT_EQ(d["authored"]["width"].as_string(), string("120px"));
    EXPECT_EQ(d["properties"]["width"].as_string(), string("120px"));
    EXPECT_FALSE(state()->modified);
}

TEST_F(CoreTest, SaveNeedsALaunchedApplication)
{
    core_->fn_detach();
    EXPECT_NE(core_->fn_save().find("\"error\""), string::npos);
}
