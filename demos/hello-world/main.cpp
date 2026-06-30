#include <Lambda.hpp>
#include <Lambda/UI/UI.hpp>

using namespace lambdaui;

int main(int argc, char *argv[]) {
    Application app(argc, argv);

    auto &w = app.createWindow({
        .size = {400, 400},
        .title = "Hello, World!",
    });

    w.setView(
        Text {
            .text = "Hello, World!",
            .font = Font::largeTitle(),
            .color = Color::primary(),
            .horizontalAlignment = HorizontalAlignment::Center,
            .verticalAlignment = VerticalAlignment::Center,
        }
    );

    return app.exec();
}
