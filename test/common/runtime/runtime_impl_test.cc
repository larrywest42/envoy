#include <memory>
#include <string>

#include "common/runtime/runtime_impl.h"
#include "common/stats/isolated_store_impl.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/filesystem/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/test_base.h"

#include "gmock/gmock.h"

using testing::_;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnNew;

namespace Envoy {
namespace Runtime {

TEST(Random, DISABLED_benchmarkRandom) {
  Runtime::RandomGeneratorImpl random;

  for (size_t i = 0; i < 1000000000; ++i) {
    random.random();
  }
}

TEST(Random, sanityCheckOfUniquenessRandom) {
  Runtime::RandomGeneratorImpl random;
  std::set<uint64_t> results;
  const size_t num_of_results = 1000000;

  for (size_t i = 0; i < num_of_results; ++i) {
    results.insert(random.random());
  }

  EXPECT_EQ(num_of_results, results.size());
}

TEST(UUID, checkLengthOfUUID) {
  RandomGeneratorImpl random;

  std::string result = random.uuid();

  size_t expected_length = 36;
  EXPECT_EQ(expected_length, result.length());
}

TEST(UUID, sanityCheckOfUniqueness) {
  std::set<std::string> uuids;
  const size_t num_of_uuids = 100000;

  RandomGeneratorImpl random;
  for (size_t i = 0; i < num_of_uuids; ++i) {
    uuids.insert(random.uuid());
  }

  EXPECT_EQ(num_of_uuids, uuids.size());
}

class DiskBackedLoaderImplTest : public TestBase {
protected:
  DiskBackedLoaderImplTest() : api_(Api::createApiForTest(store)) {}

  static void SetUpTestSuite() {
    TestEnvironment::exec(
        {TestEnvironment::runfilesPath("test/common/runtime/filesystem_setup.sh")});
  }

  void setup() {
    EXPECT_CALL(dispatcher, createFilesystemWatcher_())
        .WillOnce(ReturnNew<NiceMock<Filesystem::MockWatcher>>());
  }

  void run(const std::string& primary_dir, const std::string& override_dir) {
    loader = std::make_unique<DiskBackedLoaderImpl>(dispatcher, tls,
                                                    TestEnvironment::temporaryPath(primary_dir),
                                                    "envoy", override_dir, store, generator, *api_);
  }

  Event::MockDispatcher dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;

  Stats::IsolatedStoreImpl store;
  MockRandomGenerator generator;
  std::unique_ptr<LoaderImpl> loader;
  Api::ApiPtr api_;
};

TEST_F(DiskBackedLoaderImplTest, All) {
  setup();
  run("test/common/runtime/test_data/current", "envoy_override");

  // Basic string getting.
  EXPECT_EQ("world", loader->snapshot().get("file2"));
  EXPECT_EQ("hello\nworld", loader->snapshot().get("subdir.file3"));
  EXPECT_EQ("", loader->snapshot().get("invalid"));

  // Integer getting.
  EXPECT_EQ(1UL, loader->snapshot().getInteger("file1", 1));
  EXPECT_EQ(2UL, loader->snapshot().getInteger("file3", 1));
  EXPECT_EQ(123UL, loader->snapshot().getInteger("file4", 1));

  // Files with comments.
  EXPECT_EQ(123UL, loader->snapshot().getInteger("file5", 1));
  EXPECT_EQ("/home#about-us", loader->snapshot().get("file6"));
  EXPECT_EQ("", loader->snapshot().get("file7"));

  // Feature enablement.
  EXPECT_CALL(generator, random()).WillOnce(Return(1));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file3", 1));

  EXPECT_CALL(generator, random()).WillOnce(Return(2));
  EXPECT_FALSE(loader->snapshot().featureEnabled("file3", 1));

  // Fractional percent feature enablement
  envoy::type::FractionalPercent fractional_percent;
  fractional_percent.set_numerator(5);
  fractional_percent.set_denominator(envoy::type::FractionalPercent::TEN_THOUSAND);

  EXPECT_CALL(generator, random()).WillOnce(Return(50));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file8", fractional_percent)); // valid data

  EXPECT_CALL(generator, random()).WillOnce(Return(60));
  EXPECT_FALSE(loader->snapshot().featureEnabled("file8", fractional_percent)); // valid data

  // We currently expect that runtime values represented as fractional percents that are provided as
  // integers are parsed simply as percents (denominator of 100).
  EXPECT_CALL(generator, random()).WillOnce(Return(53));
  EXPECT_FALSE(loader->snapshot().featureEnabled("file10", fractional_percent)); // valid int data
  EXPECT_CALL(generator, random()).WillOnce(Return(51));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file10", fractional_percent)); // valid int data

  EXPECT_CALL(generator, random()).WillOnce(Return(4));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file9", fractional_percent)); // invalid proto data

  EXPECT_CALL(generator, random()).WillOnce(Return(6));
  EXPECT_FALSE(
      loader->snapshot().featureEnabled("file9", fractional_percent)); // invalid proto data

  EXPECT_CALL(generator, random()).WillOnce(Return(4));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file1", fractional_percent)); // invalid data

  EXPECT_CALL(generator, random()).WillOnce(Return(6));
  EXPECT_FALSE(loader->snapshot().featureEnabled("file1", fractional_percent)); // invalid data

  // Check stable value
  EXPECT_TRUE(loader->snapshot().featureEnabled("file3", 1, 1));
  EXPECT_FALSE(loader->snapshot().featureEnabled("file3", 1, 3));

  // Check stable value and num buckets.
  EXPECT_FALSE(loader->snapshot().featureEnabled("file4", 1, 200, 300));
  EXPECT_TRUE(loader->snapshot().featureEnabled("file4", 1, 122, 300));

  // Overrides from override dir
  EXPECT_EQ("hello override", loader->snapshot().get("file1"));
}

TEST_F(DiskBackedLoaderImplTest, GetLayers) {
  setup();
  run("test/common/runtime/test_data/current", "envoy_override");
  const auto& layers = loader->snapshot().getLayers();
  EXPECT_EQ(3, layers.size());
  EXPECT_EQ("hello", layers[0]->values().find("file1")->second.raw_string_value_);
  EXPECT_EQ("hello override", layers[1]->values().find("file1")->second.raw_string_value_);
  // Admin should be last
  EXPECT_NE(nullptr, dynamic_cast<const AdminLayer*>(layers.back().get()));
  EXPECT_TRUE(layers[2]->values().empty());

  loader->mergeValues({{"foo", "bar"}});
  // The old snapshot and its layers should have been invalidated. Refetch.
  const auto& new_layers = loader->snapshot().getLayers();
  EXPECT_EQ("bar", new_layers[2]->values().find("foo")->second.raw_string_value_);
}

TEST_F(DiskBackedLoaderImplTest, BadDirectory) {
  setup();
  run("/baddir", "/baddir");
}

TEST_F(DiskBackedLoaderImplTest, OverrideFolderDoesNotExist) {
  setup();
  run("test/common/runtime/test_data/current", "envoy_override_does_not_exist");

  EXPECT_EQ("hello", loader->snapshot().get("file1"));
}

void testNewOverrides(Loader& loader, Stats::Store& store) {
  // New string
  loader.mergeValues({{"foo", "bar"}});
  EXPECT_EQ("bar", loader.snapshot().get("foo"));
  EXPECT_EQ(1, store.gauge("runtime.admin_overrides_active").value());

  // Remove new string
  loader.mergeValues({{"foo", ""}});
  EXPECT_EQ("", loader.snapshot().get("foo"));
  EXPECT_EQ(0, store.gauge("runtime.admin_overrides_active").value());

  // New integer
  loader.mergeValues({{"baz", "42"}});
  EXPECT_EQ(42, loader.snapshot().getInteger("baz", 0));
  EXPECT_EQ(1, store.gauge("runtime.admin_overrides_active").value());

  // Remove new integer
  loader.mergeValues({{"baz", ""}});
  EXPECT_EQ(0, loader.snapshot().getInteger("baz", 0));
  EXPECT_EQ(0, store.gauge("runtime.admin_overrides_active").value());
}

TEST_F(DiskBackedLoaderImplTest, mergeValues) {
  setup();
  run("test/common/runtime/test_data/current", "envoy_override");
  testNewOverrides(*loader, store);

  // Override string
  loader->mergeValues({{"file2", "new world"}});
  EXPECT_EQ("new world", loader->snapshot().get("file2"));
  EXPECT_EQ(1, store.gauge("runtime.admin_overrides_active").value());

  // Remove overridden string
  loader->mergeValues({{"file2", ""}});
  EXPECT_EQ("world", loader->snapshot().get("file2"));
  EXPECT_EQ(0, store.gauge("runtime.admin_overrides_active").value());

  // Override integer
  loader->mergeValues({{"file3", "42"}});
  EXPECT_EQ(42, loader->snapshot().getInteger("file3", 1));
  EXPECT_EQ(1, store.gauge("runtime.admin_overrides_active").value());

  // Remove overridden integer
  loader->mergeValues({{"file3", ""}});
  EXPECT_EQ(2, loader->snapshot().getInteger("file3", 1));
  EXPECT_EQ(0, store.gauge("runtime.admin_overrides_active").value());

  // Override override string
  loader->mergeValues({{"file1", "hello overridden override"}});
  EXPECT_EQ("hello overridden override", loader->snapshot().get("file1"));
  EXPECT_EQ(1, store.gauge("runtime.admin_overrides_active").value());

  // Remove overridden override string
  loader->mergeValues({{"file1", ""}});
  EXPECT_EQ("hello override", loader->snapshot().get("file1"));
  EXPECT_EQ(0, store.gauge("runtime.admin_overrides_active").value());
}

TEST(LoaderImplTest, All) {
  MockRandomGenerator generator;
  NiceMock<ThreadLocal::MockInstance> tls;
  Stats::IsolatedStoreImpl store;
  LoaderImpl loader(generator, store, tls);
  EXPECT_EQ("", loader.snapshot().get("foo"));
  EXPECT_EQ(1UL, loader.snapshot().getInteger("foo", 1));
  EXPECT_CALL(generator, random()).WillOnce(Return(49));
  EXPECT_TRUE(loader.snapshot().featureEnabled("foo", 50));
  testNewOverrides(loader, store);
}

class DiskLayerTest : public TestBase {
protected:
  DiskLayerTest() : api_(Api::createApiForTest(store_)) {}

  Stats::IsolatedStoreImpl store_;
  Api::ApiPtr api_;
};

TEST_F(DiskLayerTest, IllegalPath) {
#ifdef WIN32
  // no illegal paths on Windows at the moment
  return;
#endif
  EXPECT_THROW_WITH_MESSAGE(DiskLayer("test", "/dev", *api_), EnvoyException, "Invalid path: /dev");
}

// Validate that we catch recursion that goes too deep in the runtime filesystem
// walk.
TEST_F(DiskLayerTest, Loop) {
  EXPECT_THROW_WITH_MESSAGE(
      DiskLayer("test", TestEnvironment::temporaryPath("test/common/runtime/test_data/loop"),
                *api_),
      EnvoyException, "Walk recursion depth exceded 16");
}

} // namespace Runtime
} // namespace Envoy
