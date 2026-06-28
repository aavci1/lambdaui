#include <doctest/doctest.h>

#include <Lambda/Graphics/TextSystem.hpp>
#include <Lambda/Reactive/Signal.hpp>
#include <Lambda/SceneGraph/SceneGraph.hpp>
#include <Lambda/SceneGraph/SceneNode.hpp>
#include <Lambda/UI/Detail/RootHolder.hpp>
#include <Lambda/UI/Element.hpp>
#include <Lambda/UI/EnvironmentBinding.hpp>
#include <Lambda/UI/MeasureContext.hpp>
#include <Lambda/UI/MountContext.hpp>
#include <Lambda/UI/MountRoot.hpp>
#include <Lambda/UI/Theme.hpp>
#include <Lambda/UI/Views/Grid.hpp>
#include <Lambda/UI/Views/HStack.hpp>
#include <Lambda/UI/Views/Rectangle.hpp>
#include <Lambda/UI/Views/ScrollView.hpp>
#include <Lambda/UI/Views/Text.hpp>
#include <Lambda/UI/Views/VStack.hpp>
#include <Lambda/UI/Views/ZStack.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

class MeasuringTextSystem final : public lambdaui::TextSystem {
public:
  std::shared_ptr<lambdaui::TextLayout const>
  layout(lambdaui::AttributedString const&, float, lambdaui::TextLayoutOptions const&) override {
    return std::make_shared<lambdaui::TextLayout>();
  }

  std::shared_ptr<lambdaui::TextLayout const>
  layout(std::string_view, lambdaui::Font const&, lambdaui::Color const&, float,
         lambdaui::TextLayoutOptions const&) override {
    return std::make_shared<lambdaui::TextLayout>();
  }

  lambdaui::Size measure(lambdaui::AttributedString const&, float,
                         lambdaui::TextLayoutOptions const&) override {
    return {80.f, 16.f};
  }

  lambdaui::Size measure(std::string_view text, lambdaui::Font const&, lambdaui::Color const&, float,
                         lambdaui::TextLayoutOptions const&) override {
    return {std::max(24.f, static_cast<float>(text.size()) * 6.f), 16.f};
  }

  std::uint32_t resolveFontId(std::string_view, float, bool) override { return 0; }

  std::vector<std::uint8_t> rasterizeGlyph(std::uint32_t, std::uint32_t, float,
                                           std::uint32_t& outWidth,
                                           std::uint32_t& outHeight,
                                           lambdaui::Point& outBearing) override {
    outWidth = 0;
    outHeight = 0;
    outBearing = {};
    return {};
  }
};

lambdaui::EnvironmentBinding testEnvironment() {
  return lambdaui::EnvironmentBinding{}.withValue<lambdaui::ThemeKey>(lambdaui::Theme::light());
}

lambdaui::LayoutConstraints fixedConstraints(lambdaui::Size size) {
  return lambdaui::LayoutConstraints{
      .maxWidth = std::max(0.f, size.width),
      .maxHeight = std::max(0.f, size.height),
      .minWidth = std::max(0.f, size.width),
      .minHeight = std::max(0.f, size.height),
  };
}

struct ElementRoot {
  lambdaui::Element element;

  lambdaui::Element body() const {
    return element;
  }
};

struct ConstraintProbe {
  lambdaui::LayoutConstraints* seen = nullptr;

  lambdaui::Size measure(lambdaui::MeasureContext&, lambdaui::LayoutConstraints const& constraints,
                         lambdaui::LayoutHints const&, lambdaui::TextSystem&) const {
    if (seen) {
      *seen = constraints;
    }
    return {
        std::max(0.f, constraints.maxWidth),
        std::max(0.f, constraints.maxHeight),
    };
  }

  std::unique_ptr<lambdaui::scenegraph::SceneNode> mount(lambdaui::MountContext&) const {
    return nullptr;
  }
};

struct NodeSnapshot {
  lambdaui::Point position{};
  lambdaui::Size size{};
};

void collectSnapshots(lambdaui::scenegraph::SceneNode const& node,
                      std::vector<NodeSnapshot>& snapshots) {
  snapshots.push_back(NodeSnapshot{.position = node.position(), .size = node.size()});
  for (auto const& child : node.children()) {
    collectSnapshots(*child, snapshots);
  }
}

std::vector<NodeSnapshot> snapshotTree(lambdaui::scenegraph::SceneNode const& root) {
  std::vector<NodeSnapshot> snapshots;
  collectSnapshots(root, snapshots);
  return snapshots;
}

void checkSameSnapshots(std::vector<NodeSnapshot> const& before,
                        std::vector<NodeSnapshot> const& after) {
  REQUIRE(after.size() == before.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    CAPTURE(i);
    CHECK(after[i].position.x == doctest::Approx(before[i].position.x).epsilon(0.01));
    CHECK(after[i].position.y == doctest::Approx(before[i].position.y).epsilon(0.01));
    CHECK(after[i].size.width == doctest::Approx(before[i].size.width).epsilon(0.01));
    CHECK(after[i].size.height == doctest::Approx(before[i].size.height).epsilon(0.01));
  }
}

void checkRelayoutParity(lambdaui::Element element, lambdaui::Size viewport = {240.f, 180.f}) {
  MeasuringTextSystem textSystem;
  lambdaui::scenegraph::SceneGraph sceneGraph;
  lambdaui::MountRoot root{
      std::make_unique<lambdaui::TypedRootHolder<ElementRoot>>(
          std::in_place, ElementRoot{std::move(element)}),
      textSystem,
      testEnvironment(),
      viewport,
  };

  root.mount(sceneGraph);
  std::vector<NodeSnapshot> const before = snapshotTree(sceneGraph.root());

  sceneGraph.root().relayout(fixedConstraints(viewport));

  checkSameSnapshots(before, snapshotTree(sceneGraph.root()));
}

lambdaui::Element box(float width, float height) {
  return lambdaui::Rectangle{}.size(width, height);
}

lambdaui::Size measureElement(lambdaui::Element element,
                              lambdaui::LayoutConstraints constraints,
                              lambdaui::LayoutHints hints = {}) {
  MeasuringTextSystem textSystem;
  lambdaui::MeasureContext ctx{textSystem, testEnvironment()};
  return element.measure(ctx, constraints, hints, textSystem);
}

struct RelayoutProbeFrame {
  lambdaui::Element child;
  int* relayouts = nullptr;

  lambdaui::Size measure(lambdaui::MeasureContext& ctx, lambdaui::LayoutConstraints const&,
                         lambdaui::LayoutHints const&, lambdaui::TextSystem&) const {
    ctx.advanceChildSlot();
    return {40.f, 30.f};
  }

  std::unique_ptr<lambdaui::scenegraph::SceneNode> mount(lambdaui::MountContext& ctx) const {
    auto group = std::make_unique<lambdaui::scenegraph::SceneNode>(
        lambdaui::Rect{0.f, 0.f, 40.f, 30.f});
    lambdaui::MountContext childCtx = ctx.childWithSharedScope(fixedConstraints({40.f, 30.f}), ctx.hints());
    auto childNode = child.mount(childCtx);
    lambdaui::scenegraph::SceneNode* rawChild = childNode.get();
    if (childNode) {
      group->appendChild(std::move(childNode));
    }
    auto* rawGroup = group.get();
    rawGroup->setLayoutConstraints(ctx.constraints());
    rawGroup->setRelayout([rawGroup, rawChild, relayouts = relayouts](
                              lambdaui::LayoutConstraints const&) {
      if (relayouts) {
        ++*relayouts;
      }
      if (rawChild) {
        rawChild->relayout(fixedConstraints({40.f, 30.f}));
      }
      rawGroup->setSize({40.f, 30.f});
    });
    return group;
  }
};

struct CollapsedMountedBox {
  static constexpr bool mountsWhenCollapsed = true;

  lambdaui::Size mountedSize{};

  lambdaui::Size measure(lambdaui::MeasureContext&, lambdaui::LayoutConstraints const&,
                         lambdaui::LayoutHints const&, lambdaui::TextSystem&) const {
    return {};
  }

  std::unique_ptr<lambdaui::scenegraph::SceneNode> mount(lambdaui::MountContext&) const {
    return std::make_unique<lambdaui::scenegraph::SceneNode>(
        lambdaui::Rect{0.f, 0.f, mountedSize.width, mountedSize.height});
  }
};

} // namespace

TEST_CASE("padding larger than available size clamps child constraints non-negative") {
  MeasuringTextSystem textSystem;
  lambdaui::MeasureContext ctx{textSystem, testEnvironment()};
  lambdaui::LayoutConstraints seen{};
  lambdaui::Element element = lambdaui::Element{ConstraintProbe{.seen = &seen}}
                                  .padding(lambdaui::EdgeInsets{
                                      .top = 20.f,
                                      .right = 24.f,
                                      .bottom = 20.f,
                                      .left = 24.f,
                                  });

  lambdaui::Size const measured = element.measure(
      ctx,
      lambdaui::LayoutConstraints{
          .maxWidth = 30.f,
          .maxHeight = 28.f,
          .minWidth = 0.f,
          .minHeight = 0.f,
      },
      {},
      textSystem);

  CHECK(seen.maxWidth == doctest::Approx(0.f));
  CHECK(seen.maxHeight == doctest::Approx(0.f));
  CHECK(seen.minWidth == doctest::Approx(0.f));
  CHECK(seen.minHeight == doctest::Approx(0.f));
  CHECK(measured.width >= 0.f);
  CHECK(measured.height >= 0.f);
}

TEST_CASE("HStack and VStack flex gating matches under loose finite main-axis caps") {
  auto makeHStack = [] {
    return lambdaui::Element{lambdaui::HStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };
  auto makeVStack = [] {
    return lambdaui::Element{lambdaui::VStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };

  lambdaui::Size const horizontal = measureElement(
      makeHStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = 100.f,
          .maxHeight = std::numeric_limits<float>::infinity(),
          .minWidth = 0.f,
          .minHeight = 0.f,
      });
  lambdaui::Size const vertical = measureElement(
      makeVStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = std::numeric_limits<float>::infinity(),
          .maxHeight = 100.f,
          .minWidth = 0.f,
          .minHeight = 0.f,
      });

  CHECK(horizontal.width == doctest::Approx(vertical.height));
  CHECK(horizontal.width == doctest::Approx(10.f));
}

TEST_CASE("HStack and VStack flex gating still fills tight finite main-axis constraints") {
  auto makeHStack = [] {
    return lambdaui::Element{lambdaui::HStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };
  auto makeVStack = [] {
    return lambdaui::Element{lambdaui::VStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };

  lambdaui::Size const horizontal = measureElement(
      makeHStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = 100.f,
          .maxHeight = std::numeric_limits<float>::infinity(),
          .minWidth = 100.f,
          .minHeight = 0.f,
      });
  lambdaui::Size const vertical = measureElement(
      makeVStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = std::numeric_limits<float>::infinity(),
          .maxHeight = 100.f,
          .minWidth = 0.f,
          .minHeight = 100.f,
      });

  CHECK(horizontal.width == doctest::Approx(vertical.height));
  CHECK(horizontal.width == doctest::Approx(100.f));
}

TEST_CASE("HStack and VStack flex gating stays natural under infinite main-axis constraints") {
  auto makeHStack = [] {
    return lambdaui::Element{lambdaui::HStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };
  auto makeVStack = [] {
    return lambdaui::Element{lambdaui::VStack{
        .children = lambdaui::children(box(10.f, 10.f).flex(1.f)),
    }};
  };

  lambdaui::Size const horizontal = measureElement(
      makeHStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = std::numeric_limits<float>::infinity(),
          .maxHeight = std::numeric_limits<float>::infinity(),
          .minWidth = 0.f,
          .minHeight = 0.f,
      });
  lambdaui::Size const vertical = measureElement(
      makeVStack(),
      lambdaui::LayoutConstraints{
          .maxWidth = std::numeric_limits<float>::infinity(),
          .maxHeight = std::numeric_limits<float>::infinity(),
          .minWidth = 0.f,
          .minHeight = 0.f,
      });

  CHECK(horizontal.width == doctest::Approx(vertical.height));
  CHECK(horizontal.width == doctest::Approx(10.f));
}

TEST_CASE("stack, grid, and scroll mount geometry matches same-constraint relayout") {
  checkRelayoutParity(lambdaui::VStack{
      .spacing = 6.f,
      .alignment = lambdaui::Alignment::Center,
      .children = lambdaui::children(box(30.f, 12.f), box(54.f, 20.f), box(18.f, 10.f)),
  });

  checkRelayoutParity(lambdaui::HStack{
      .spacing = 7.f,
      .alignment = lambdaui::Alignment::Center,
      .children = lambdaui::children(box(20.f, 18.f), box(35.f, 12.f), box(22.f, 24.f)),
  });

  checkRelayoutParity(lambdaui::ZStack{
      .horizontalAlignment = lambdaui::Alignment::Center,
      .verticalAlignment = lambdaui::Alignment::Center,
      .children = lambdaui::children(box(80.f, 40.f), box(30.f, 16.f)),
  });

  checkRelayoutParity(lambdaui::Grid{
      .columns = 2,
      .horizontalSpacing = 5.f,
      .verticalSpacing = 9.f,
      .horizontalAlignment = lambdaui::Alignment::Center,
      .verticalAlignment = lambdaui::Alignment::Center,
      .children = lambdaui::children(box(42.f, 18.f), box(28.f, 22.f), box(34.f, 16.f)),
  });

  checkRelayoutParity(lambdaui::ScrollView{
      .axis = lambdaui::ScrollAxis::Vertical,
      .children = lambdaui::children(lambdaui::VStack{
          .spacing = 4.f,
          .alignment = lambdaui::Alignment::Stretch,
          .children = lambdaui::children(box(30.f, 24.f), box(40.f, 28.f), box(50.f, 32.f)),
      }),
  });

  checkRelayoutParity(lambdaui::VStack{
      .spacing = 10.f,
      .alignment = lambdaui::Alignment::Stretch,
      .children = lambdaui::children(
          lambdaui::HStack{
              .spacing = 3.f,
              .alignment = lambdaui::Alignment::Center,
              .children = lambdaui::children(box(24.f, 16.f), box(42.f, 20.f)),
          },
          lambdaui::ZStack{
              .horizontalAlignment = lambdaui::Alignment::End,
              .verticalAlignment = lambdaui::Alignment::Center,
              .children = lambdaui::children(box(60.f, 24.f), box(20.f, 10.f)),
          },
          lambdaui::Grid{
              .columns = 2,
              .children = lambdaui::children(box(20.f, 14.f), box(30.f, 18.f), box(26.f, 12.f)),
          }),
  });
}

TEST_CASE("mounted collapsed stack children keep active siblings in the same position on relayout") {
  checkRelayoutParity(lambdaui::VStack{
      .spacing = 5.f,
      .alignment = lambdaui::Alignment::Start,
      .children = lambdaui::children(
          box(16.f, 10.f),
          CollapsedMountedBox{.mountedSize = {12.f, 14.f}},
          box(20.f, 8.f)),
  });

  checkRelayoutParity(lambdaui::HStack{
      .spacing = 6.f,
      .alignment = lambdaui::Alignment::Start,
      .children = lambdaui::children(
          box(10.f, 16.f),
          CollapsedMountedBox{.mountedSize = {18.f, 12.f}},
          box(8.f, 20.f)),
  });
}

TEST_CASE("reactive text geometry matches forced relayout geometry") {
  lambdaui::Reactive::Signal<std::string> label{"short"};
  struct Root {
    lambdaui::Reactive::Signal<std::string> label;

    lambdaui::Element body() const {
      return lambdaui::Text{
          .text = [label = label] {
            return label.get();
          },
      };
    }
  };

  MeasuringTextSystem textSystem;
  lambdaui::scenegraph::SceneGraph sceneGraph;
  lambdaui::MountRoot root{
      std::make_unique<lambdaui::TypedRootHolder<Root>>(std::in_place, Root{label}),
      textSystem,
      testEnvironment(),
      lambdaui::Size{240.f, 180.f},
  };

  root.mount(sceneGraph);
  label.set("a much longer reactive label");
  std::vector<NodeSnapshot> const afterSignal = snapshotTree(sceneGraph.root());

  sceneGraph.root().relayout(fixedConstraints({240.f, 180.f}));

  checkSameSnapshots(afterSignal, snapshotTree(sceneGraph.root()));
}

TEST_CASE("reactive text reflow pushes HStack siblings") {
  lambdaui::Reactive::Signal<std::string> label{"a"};
  struct Root {
    lambdaui::Reactive::Signal<std::string> label;

    lambdaui::Element body() const {
      return lambdaui::HStack{
          .spacing = 4.f,
          .alignment = lambdaui::Alignment::Center,
          .children = lambdaui::children(
              lambdaui::Text{
                  .text = [label = label] {
                    return label.get();
                  },
              },
              lambdaui::Text{.text = std::string{"fixed"}}),
      };
    }
  };

  MeasuringTextSystem textSystem;
  lambdaui::scenegraph::SceneGraph sceneGraph;
  lambdaui::MountRoot root{
      std::make_unique<lambdaui::TypedRootHolder<Root>>(std::in_place, Root{label}),
      textSystem,
      testEnvironment(),
      lambdaui::Size{240.f, 180.f},
  };

  root.mount(sceneGraph);
  REQUIRE(sceneGraph.root().children().size() == 2);
  float const initialSecondX = sceneGraph.root().children()[1]->position().x;

  label.set("substantially wider");

  // Mirrors kTextContentChangesTriggerReflow.
  CHECK(sceneGraph.root().children()[1]->position().x > initialSecondX);
}

TEST_CASE("reactive text reflow stays scoped to the containing stack") {
  lambdaui::Reactive::Signal<std::string> label{"a"};
  int unrelatedRelayouts = 0;
  struct Root {
    lambdaui::Reactive::Signal<std::string> label;
    int* unrelatedRelayouts = nullptr;

    lambdaui::Element body() const {
      return lambdaui::ZStack{
          .horizontalAlignment = lambdaui::Alignment::Start,
          .verticalAlignment = lambdaui::Alignment::Start,
          .children = lambdaui::children(
              lambdaui::HStack{
                  .spacing = 4.f,
                  .alignment = lambdaui::Alignment::Center,
                  .children = lambdaui::children(
                      lambdaui::Text{
                          .text = [label = label] {
                            return label.get();
                          },
                      },
                      lambdaui::Text{.text = std::string{"fixed"}}),
              },
              RelayoutProbeFrame{
                  .child = lambdaui::Element{lambdaui::Rectangle{}}
                               .size(10.f, 10.f),
                  .relayouts = unrelatedRelayouts,
              }),
      };
    }
  };

  MeasuringTextSystem textSystem;
  lambdaui::scenegraph::SceneGraph sceneGraph;
  lambdaui::MountRoot root{
      std::make_unique<lambdaui::TypedRootHolder<Root>>(
          std::in_place, Root{label, &unrelatedRelayouts}),
      textSystem,
      testEnvironment(),
      lambdaui::Size{240.f, 180.f},
  };

  root.mount(sceneGraph);
  REQUIRE(sceneGraph.root().children().size() == 2);
  unrelatedRelayouts = 0;
  label.set("substantially wider");

  CHECK(unrelatedRelayouts == 0);
}
