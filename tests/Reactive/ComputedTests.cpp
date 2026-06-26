#include <Lambda/Reactive/Computed.hpp>
#include <Lambda/Reactive/Effect.hpp>
#include <Lambda/Reactive/Signal.hpp>

#include <doctest/doctest.h>

#include <vector>

using namespace lambdaui::Reactive;

TEST_CASE("Reactive Computed recomputes lazily") {
  Signal<int> source(2);
  int runs = 0;
  Computed<int> doubled([&] {
    ++runs;
    return source.get() * 2;
  });

  CHECK(doubled.get() == 4);
  CHECK(runs == 1);

  source.set(3);
  CHECK(runs == 1);
  CHECK(doubled.get() == 6);
  CHECK(runs == 2);
}

TEST_CASE("Reactive Computed supports transitive and dynamic dependencies") {
  Signal<int> source(3);
  Computed<int> doubled([&] {
    return source.get() * 2;
  });
  Computed<int> plusOne([&] {
    return doubled.get() + 1;
  });

  CHECK(plusOne.get() == 7);
  source.set(4);
  CHECK(plusOne.get() == 9);

  Signal<bool> useLeft(true);
  Signal<int> left(10);
  Signal<int> right(20);
  int runs = 0;
  Computed<int> selected([&] {
    ++runs;
    return useLeft.get() ? left.get() : right.get();
  });

  CHECK(selected.get() == 10);
  right.set(21);
  CHECK(selected.get() == 10);
  CHECK(runs == 1);

  useLeft.set(false);
  CHECK(selected.get() == 21);
  CHECK(runs == 2);

  left.set(11);
  CHECK(selected.get() == 21);
  CHECK(runs == 2);
}

TEST_CASE("Reactive Computed skips downstream recompute when intermediate value is unchanged") {
  Signal<int> source(1);
  int bucketRuns = 0;
  int derivedRuns = 0;
  int effectRuns = 0;
  int observed = 0;

  Computed<int> bucket([&] {
    ++bucketRuns;
    return source.get() % 2;
  });
  Computed<int> derived([&] {
    ++derivedRuns;
    return bucket.get() * 10;
  });
  Effect effect([&] {
    ++effectRuns;
    observed = derived.get();
  });

  CHECK(observed == 10);
  CHECK(bucketRuns == 1);
  CHECK(derivedRuns == 1);
  CHECK(effectRuns == 1);

  source.set(3);
  CHECK(observed == 10);
  CHECK(bucketRuns == 2);
  CHECK(derivedRuns == 1);
  CHECK(effectRuns == 1);

  source.set(4);
  CHECK(observed == 0);
  CHECK(bucketRuns == 3);
  CHECK(derivedRuns == 2);
  CHECK(effectRuns == 2);
}

#if LAMBDAUI_PROFILE_REACTIVE
TEST_CASE("Reactive Computed reuses stable wide source order without source scans") {
  std::vector<Signal<int>> sources;
  sources.reserve(64);
  int expected = 0;
  for (int i = 0; i < 64; ++i) {
    sources.emplace_back(i);
    expected += i;
  }

  int runs = 0;
  Computed<int> sum([&] {
    ++runs;
    int total = 0;
    for (Signal<int> const& source : sources) {
      total += source.get();
    }
    return total;
  });

  CHECK(sum.get() == expected);
  CHECK(runs == 1);

  detail::debugResetSourceScanStepCount();
  sources[0].set(100);
  expected += 100;

  CHECK(sum.get() == expected);
  CHECK(runs == 2);
  CHECK(detail::debugSourceScanStepCount() <= sources.size());
}
#endif

TEST_CASE("Reactive Computed preserves dependencies when read order changes") {
  Signal<bool> reverse(false);
  Signal<int> a(1);
  Signal<int> b(2);
  Signal<int> c(3);
  int runs = 0;

  Computed<int> sum([&] {
    ++runs;
    if (reverse.get()) {
      return c.get() + b.get() + a.get();
    }
    return a.get() + b.get() + c.get();
  });

  CHECK(sum.get() == 6);
  CHECK(runs == 1);

  reverse.set(true);
  CHECK(sum.get() == 6);
  CHECK(runs == 2);

  a.set(10);
  CHECK(sum.get() == 15);
  CHECK(runs == 3);

  b.set(20);
  CHECK(sum.get() == 33);
  CHECK(runs == 4);

  c.set(30);
  CHECK(sum.get() == 60);
  CHECK(runs == 5);
}
