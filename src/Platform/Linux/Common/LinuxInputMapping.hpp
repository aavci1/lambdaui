#pragma once

#include <Lambda/UI/Input.hpp>

#include <cstdint>

namespace lambdaui::linux_platform {

inline constexpr std::uint32_t kEvdevButtonLeft = 0x110;
inline constexpr std::uint32_t kEvdevButtonRight = 0x111;
inline constexpr std::uint32_t kEvdevButtonMiddle = 0x112;

inline constexpr std::uint8_t kPressedPrimaryButton = 1u;
inline constexpr std::uint8_t kPressedSecondaryButton = 2u;
inline constexpr std::uint8_t kPressedMiddleButton = 4u;

constexpr MouseButton mouseButtonFromLinuxButton(std::uint32_t button) noexcept {
  if (button == kEvdevButtonLeft) return MouseButton::Left;
  if (button == kEvdevButtonRight) return MouseButton::Right;
  if (button == kEvdevButtonMiddle) return MouseButton::Middle;
  return MouseButton::Other;
}

constexpr std::uint8_t mouseButtonMaskBitFromLinuxButton(std::uint32_t button) noexcept {
  if (button == kEvdevButtonLeft) return kPressedPrimaryButton;
  if (button == kEvdevButtonRight) return kPressedSecondaryButton;
  if (button == kEvdevButtonMiddle) return kPressedMiddleButton;
  return 0u;
}

} // namespace lambdaui::linux_platform
