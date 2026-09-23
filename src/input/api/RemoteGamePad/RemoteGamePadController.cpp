#include "input/api/RemoteGamePad/RemoteGamePadController.h"

RemoteGamePadController::RemoteGamePadController()
	: base_type("remote-gamepad-v1", "Remote GamePad")
{
	// A remote controller is neutral before connection. Do not calibrate a held
	// button or stick as the baseline when the first STATE arrives.
	m_default_state = {};
	m_is_calibrated = true;
	m_settings.motion = true;
}

ControllerState RemoteGamePadController::raw_state()
{
	ControllerState result{};
	const auto snapshot = m_provider->GetSnapshot();
	if (!snapshot.connected)
		return result;
	for (uint32_t bit = 0; bit <= 18; ++bit)
		if (snapshot.state.buttons & (uint32_t(1) << bit))
			result.buttons.SetButtonState(kButton0 + bit, true);
	result.axis = {snapshot.state.leftX / 32767.0f, snapshot.state.leftY / 32767.0f};
	result.rotation = {snapshot.state.rightX / 32767.0f, snapshot.state.rightY / 32767.0f};
	return result;
}

bool RemoteGamePadController::has_motion()
{
	const auto state = m_provider->GetSnapshot();
	return state.connected && (state.features & RemoteGamePadProtocol::kMotion);
}

bool RemoteGamePadController::has_position()
{
	const auto state = m_provider->GetSnapshot();
	return state.connected && (state.features & RemoteGamePadProtocol::kTouch) && state.state.touchActive;
}

glm::vec2 RemoteGamePadController::get_position()
{
	const auto state = m_provider->GetSnapshot();
	// VPADController::update_touch expects Y measured from the top of the image.
	return {state.state.touchX / 65535.0f, state.state.touchY / 65535.0f};
}

PositionVisibility RemoteGamePadController::GetPositionVisibility()
{
	return has_position() ? PositionVisibility::FULL : PositionVisibility::NONE;
}

bool RemoteGamePadController::has_rumble()
{
	const auto state = m_provider->GetSnapshot();
	return state.connected && (state.features & RemoteGamePadProtocol::kRumble);
}

std::string RemoteGamePadController::get_button_name(uint64 button) const
{
	static constexpr std::array<std::string_view, 19> names = {
		"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "Home",
		"D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left", "Stick L", "Stick R", "Screen", "Mic"
	};
	if (button <= kButton18)
		return std::string(names[button]);
	return base_type::get_button_name(button);
}
