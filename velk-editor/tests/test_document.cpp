#include "document.h"

#include <gtest/gtest.h>

#include <cstring>

using namespace velk;
using namespace velk::editor;

namespace {

const string_view kFile("file:///x/scene.velk");

const char* kScene = R"({
  "version": 1,
  "objects": [ { "id": "a", "class": "x.A", "properties": { "w": 1, "f": { "value": 2, "flags": 1 } } },
               { "id": "b", "class": "x.A" } ],
  "attachments": [ { "id": "v", "targets": ["a"], "class": "x.V", "properties": { "c": { "bind": "a.w" } } } ]
})";

Document loaded()
{
    Document d;
    string error;
    EXPECT_TRUE(d.load_text(kFile, string_view(kScene, std::strlen(kScene)), error)) << error.c_str();
    return d;
}

} // namespace

TEST(Document, FindsEntriesAndValues)
{
    auto d = loaded();
    EXPECT_TRUE(d.has(kFile));
    EXPECT_TRUE(d.has_entry(kFile, "objects/a"));
    EXPECT_TRUE(d.has_entry(kFile, "attachments/v"));
    EXPECT_FALSE(d.has_entry(kFile, "objects/missing"));
    EXPECT_FALSE(d.has_entry(kFile, "objects"));
    ASSERT_NE(d.get(kFile, "objects/a", "w"), nullptr);
    EXPECT_EQ(d.get(kFile, "objects/a", "w")->as_number(), 1.0);
    EXPECT_EQ(d.get(kFile, "objects/b", "w"), nullptr);
    EXPECT_FALSE(d.modified(kFile));
}

TEST(Document, PutAddsReplacesAndRemoves)
{
    auto d = loaded();
    const JsonValue three = JsonValue::number(3);
    EXPECT_TRUE(d.put(kFile, "objects/a", "w", &three));
    EXPECT_EQ(d.get(kFile, "objects/a", "w")->as_number(), 3.0);
    EXPECT_TRUE(d.put(kFile, "objects/b", "w", &three)); // b has no properties yet
    EXPECT_EQ(d.get(kFile, "objects/b", "w")->as_number(), 3.0);
    EXPECT_TRUE(d.put(kFile, "objects/b", "w", nullptr));
    EXPECT_EQ(d.properties(kFile, "objects/b"), nullptr);
    EXPECT_FALSE(d.put(kFile, "objects/missing", "w", &three));
    EXPECT_TRUE(d.modified(kFile));
}

TEST(Document, ObjectFormKeepsFlags)
{
    auto d = loaded();
    const JsonValue five = JsonValue::number(5);
    EXPECT_TRUE(d.put(kFile, "objects/a", "f", &five));
    auto* f = d.get(kFile, "objects/a", "f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ((*f)["value"].as_number(), 5.0);
    EXPECT_EQ((*f)["flags"].as_number(), 1.0);
}

TEST(Document, TextKeepsOrderAndUntouchedValues)
{
    auto d = loaded();
    const JsonValue s = JsonValue::string("x");
    d.put(kFile, "objects/a", "w", &s);
    auto text = d.text(kFile);
    EXPECT_NE(text.find("\"w\": \"x\""), string::npos) << text.c_str();
    EXPECT_NE(text.find("{ \"bind\": \"a.w\" }"), string::npos) << text.c_str();
    EXPECT_LT(text.find("\"objects\""), text.find("\"attachments\""));
}

TEST(Document, SourcePathDropsScheme)
{
    EXPECT_EQ(source_path("file:///C:/x/a.velk"), string("C:/x/a.velk"));
    EXPECT_EQ(source_path("file://C:/x/a.velk"), string("C:/x/a.velk"));
    EXPECT_EQ(source_path("file:///home/a.velk"), string("/home/a.velk"));
    EXPECT_EQ(source_path("C:/x/a.velk"), string("C:/x/a.velk"));
}
