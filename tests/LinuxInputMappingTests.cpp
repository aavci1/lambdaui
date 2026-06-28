#include <doctest/doctest.h>

#include "Platform/Linux/Common/LinuxInputMapping.hpp"

TEST_CASE("Linux input mapping converts evdev buttons to public mouse buttons") {
  using namespace lambdaui;

  CHECK(linux_platform::mouseButtonFromLinuxButton(linux_platform::kEvdevButtonLeft) == MouseButton::Left);
  CHECK(linux_platform::mouseButtonFromLinuxButton(linux_platform::kEvdevButtonRight) == MouseButton::Right);
  CHECK(linux_platform::mouseButtonFromLinuxButton(linux_platform::kEvdevButtonMiddle) == MouseButton::Middle);
  CHECK(linux_platform::mouseButtonFromLinuxButton(0x2ff) == MouseButton::Other);
}

TEST_CASE("Linux input mapping exposes stable pressed button mask bits") {
  using namespace lambdaui;

  CHECK(linux_platform::mouseButtonMaskBitFromLinuxButton(linux_platform::kEvdevButtonLeft) ==
        linux_platform::kPressedPrimaryButton);
  CHECK(linux_platform::mouseButtonMaskBitFromLinuxButton(linux_platform::kEvdevButtonRight) ==
        linux_platform::kPressedSecondaryButton);
  CHECK(linux_platform::mouseButtonMaskBitFromLinuxButton(linux_platform::kEvdevButtonMiddle) ==
        linux_platform::kPressedMiddleButton);
  CHECK(linux_platform::mouseButtonMaskBitFromLinuxButton(0x2ff) == 0u);
}
