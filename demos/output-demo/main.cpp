#include <Lambda.hpp>
#include <Lambda/UI/Theme.hpp>
#include <Lambda/UI/UI.hpp>
#include <Lambda/UI/Views/Text.hpp>
#include <Lambda/UI/Views/VStack.hpp>

#include <cstddef>
#include <string>
#include <vector>

using namespace lambdaui;

struct OutputRoot {
  std::vector<std::string> outputs;

  auto body() const {
    std::string outputList;
    if (outputs.empty()) {
      outputList = "No named outputs reported";
    } else {
      for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (index > 0) outputList += "\n";
        outputList += outputs[index];
      }
    }

    return VStack{
        .spacing = 12.f,
        .alignment = Alignment::Center,
        .children = children(
            Text{
                .text = "Reported Outputs",
                .font = Font::largeTitle(),
                .color = Color::primary(),
                .horizontalAlignment = HorizontalAlignment::Center,
            },
            Text{
                .text = outputList,
                .font = Font::body(),
                .color = Color::secondary(),
                .horizontalAlignment = HorizontalAlignment::Center,
            })
    };
  }
};

int main(int argc, char* argv[]) {
  Application app(argc, argv);
  app.setName("Output Demo");

  std::vector<std::string> outputs = app.availableOutputs();

  auto& window = app.createWindow<Window>({
      .size = {640, 420},
      .title = "Output Demo",
  });
  window.setView(OutputRoot{.outputs = outputs});

  return app.exec();
}
