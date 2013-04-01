// Copyright (C) 2013  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <datasrc/cache_config.h>
#include <datasrc/tests/mock_client.h>

#include <cc/data.h>
#include <dns/name.h>

#include <gtest/gtest.h>

using namespace isc::datasrc;
using namespace isc::data;
using namespace isc::dns;
using isc::datasrc::unittest::MockDataSourceClient;
using isc::datasrc::internal::CacheConfig;
using isc::datasrc::internal::CacheConfigError;

namespace {

const char* zones[] = {
    "example.org.",
    "example.com.",
    NULL
};

class CacheConfigTest : public ::testing::Test {
protected:
    CacheConfigTest() :
        mock_client_(zones),
        master_config_(Element::fromJSON(
                           "{\"cache-enable\": true,"
                           " \"params\": "
                           "  {\".\": \"" TEST_DATA_DIR "/root.zone\"}"
                           "}")),
        mock_config_(Element::fromJSON("{\"cache-enable\": true,"
                                       " \"cache-zones\": [\".\"]}"))
    {}

    MockDataSourceClient mock_client_;
    const ConstElementPtr master_config_; // valid config for MasterFiles
    const ConstElementPtr mock_config_; // valid config for MasterFiles
};

TEST_F(CacheConfigTest, constructMasterFiles) {
    // A simple case: configuring a MasterFiles table with a single zone
    const CacheConfig cache_conf("MasterFiles", 0, *master_config_, true);
    // getZoneConfig() returns a map containing exactly one entry
    // corresponding to the root zone information in the configuration.
    EXPECT_EQ(1, cache_conf.getZoneConfig().size());
    EXPECT_EQ(Name::ROOT_NAME(), cache_conf.getZoneConfig().begin()->first);
    EXPECT_EQ(TEST_DATA_DIR "/root.zone",
              cache_conf.getZoneConfig().begin()->second);

    // With multiple zones.  There shouldn't be anything special, so we
    // only check the size of getZoneConfig.  Note that the constructor
    // doesn't check if the file exists, so they can be anything.
    const ConstElementPtr config_elem_multi(
        Element::fromJSON("{\"cache-enable\": true,"
                          " \"params\": "
                          "{\"example.com\": \"file1\","
                          " \"example.org\": \"file2\","
                          " \"example.info\": \"file3\"}"
                          "}"));
    EXPECT_EQ(3, CacheConfig("MasterFiles", 0, *config_elem_multi, true).
              getZoneConfig().size());

    // A bit unusual, but acceptable case: empty parameters, so no zones.
    EXPECT_TRUE(CacheConfig("MasterFiles", 0,
                            *Element::fromJSON("{\"cache-enable\": true,"
                                               " \"params\": {}}"), true).
                getZoneConfig().empty());
}

TEST_F(CacheConfigTest, badConstructMasterFiles) {
    // no "params"
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"cache-enable\": true}"),
                             true),
                 isc::data::TypeError);

    // no "cache-enable"
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"params\": {}}"), true),
                 CacheConfigError);
    // cache disabled for MasterFiles
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"cache-enable\": false,"
                                                " \"params\": {}}"), true),
                 CacheConfigError);
    // cache enabled but not "allowed"
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"cache-enable\": false,"
                                                " \"params\": {}}"), false),
                 CacheConfigError);
    // type error for cache-enable
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"cache-enable\": 1,"
                                                " \"params\": {}}"), true),
                 isc::data::TypeError);

    // "params" is not a map
    EXPECT_THROW(CacheConfig("MasterFiles", 0,
                             *Element::fromJSON("{\"cache-enable\": true,"
                                                " \"params\": []}"), true),
                 isc::data::TypeError);

    // bogus zone name
    const ConstElementPtr bad_config(Element::fromJSON(
                                         "{\"cache-enable\": true,"
                                         " \"params\": "
                                         "{\"bad..name\": \"file1\"}}"));
    EXPECT_THROW(CacheConfig("MasterFiles", 0, *bad_config, true),
                 isc::dns::EmptyLabel);

    // file name is not a string
    const ConstElementPtr bad_config2(Element::fromJSON(
                                          "{\"cache-enable\": true,"
                                          " \"params\": {\".\": 1}}"));
    EXPECT_THROW(CacheConfig("MasterFiles", 0, *bad_config2, true),
                 isc::data::TypeError);

    // Specify data source client (must be null for MasterFiles)
    EXPECT_THROW(CacheConfig("MasterFiles", &mock_client_,
                             *Element::fromJSON("{\"cache-enable\": true,"
                                                " \"params\": {}}"), true),
                 isc::InvalidParameter);
}

TEST_F(CacheConfigTest, constructWithMock) {
    // Performing equivalent set of tests as constructMasterFiles

    // Configure with a single zone.
    const CacheConfig cache_conf("mock", &mock_client_, *mock_config_, true);
    EXPECT_EQ(1, cache_conf.getZoneConfig().size());
    EXPECT_EQ(Name::ROOT_NAME(), cache_conf.getZoneConfig().begin()->first);
    EXPECT_EQ("", cache_conf.getZoneConfig().begin()->second);
    EXPECT_TRUE(cache_conf.isEnabled());

    // Configure with multiple zones.
    const ConstElementPtr config_elem_multi(
        Element::fromJSON("{\"cache-enable\": true,"
                          " \"cache-zones\": "
                          "[\"example.com\", \"example.org\",\"example.info\"]"
                          "}"));
    EXPECT_EQ(3, CacheConfig("mock", &mock_client_, *config_elem_multi, true).
              getZoneConfig().size());

    // Empty
    EXPECT_TRUE(CacheConfig("mock", &mock_client_,
                            *Element::fromJSON("{\"cache-enable\": true,"
                                               " \"cache-zones\": []}"), true).
                getZoneConfig().empty());

    // disabled.  value of cache-zones are ignored.
    const ConstElementPtr config_elem_disabled(
        Element::fromJSON("{\"cache-enable\": false,"
                          " \"cache-zones\": [\"example.com\"]}"));
    EXPECT_FALSE(CacheConfig("mock", &mock_client_, *config_elem_disabled,
                             true).isEnabled());
    // enabled but not "allowed".  same effect.
    EXPECT_FALSE(CacheConfig("mock", &mock_client_,
                             *Element::fromJSON("{\"cache-enable\": true,"
                                                " \"cache-zones\": []}"),
                             false).isEnabled());
}

TEST_F(CacheConfigTest, badConstructWithMock) {
    // no "cache-zones" (may become valid in future, but for now "notimp")
    EXPECT_THROW(CacheConfig("mock", &mock_client_,
                             *Element::fromJSON("{\"cache-enable\": true}"),
                             true),
                 isc::NotImplemented);

    // "cache-zones" is not a list
    EXPECT_THROW(CacheConfig("mock", &mock_client_,
                             *Element::fromJSON("{\"cache-enable\": true,"
                                                " \"cache-zones\": {}}"),
                             true),
                 isc::data::TypeError);

    // "cache-zone" entry is not a string
    EXPECT_THROW(CacheConfig("mock", &mock_client_,
                             *Element::fromJSON("{\"cache-enable\": true,"
                                                " \"cache-zones\": [1]}"),
                             true),
                 isc::data::TypeError);

    // bogus zone name
    const ConstElementPtr bad_config(Element::fromJSON(
                                         "{\"cache-enable\": true,"
                                         " \"cache-zones\": [\"bad..\"]}"));
    EXPECT_THROW(CacheConfig("mock", &mock_client_, *bad_config, true),
                 isc::dns::EmptyLabel);

    // duplicate zone name
    const ConstElementPtr dup_config(Element::fromJSON(
                                         "{\"cache-enable\": true,"
                                         " \"cache-zones\": "
                                         " [\"example\", \"example\"]}"));
    EXPECT_THROW(CacheConfig("mock", &mock_client_, *dup_config, true),
                 isc::InvalidParameter);

    // datasrc is null
    EXPECT_THROW(CacheConfig("mock", 0, *mock_config_, true),
                 isc::InvalidParameter);
}

TEST_F(CacheConfigTest, getSegmentType) {
    // Default type
    EXPECT_EQ("local",
              CacheConfig("MasterFiles", 0,
                          *master_config_, true).getSegmentType());

    // If we explicitly configure it, that value should be used.
    ConstElementPtr config(Element::fromJSON("{\"cache-enable\": true,"
                                             " \"cache-type\": \"mapped\","
                                             " \"params\": {}}" ));
    EXPECT_EQ("mapped",
              CacheConfig("MasterFiles", 0, *config, true).getSegmentType());

    // Wrong types: should be rejected at construction time
    ConstElementPtr badconfig(Element::fromJSON("{\"cache-enable\": true,"
                                                " \"cache-type\": 1,"
                                                " \"params\": {}}"));
    EXPECT_THROW(CacheConfig("MasterFiles", 0, *badconfig, true),
                 isc::data::TypeError);
}

}
