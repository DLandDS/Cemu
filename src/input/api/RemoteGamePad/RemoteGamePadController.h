#pragma once

#include "input/api/Controller.h"
#include "input/api/RemoteGamePad/RemoteGamePadProvider.h"

class RemoteGamePadController final : public Controller<RemoteGamePadProvider>
{
public:
	RemoteGamePadController();
	InputAPI::Type api() const override { return InputAPI::RemoteGamePad; }
	std::string_view api_name() const override { return InputAPI::to_string(api()); }
	bool is_connected() override { return m_provider->GetSnapshot().connected; }
	ControllerState raw_state() override;
	bool has_motion() override;
	MotionSample get_motion_sample() override { return m_provider->GetSnapshot().motion; }
	bool has_position() override;
	glm::vec2 get_position() override;
	PositionVisibility GetPositionVisibility() override;
	bool has_rumble() override;
	void start_rumble() override { m_provider->SetRumble(true); }
	void stop_rumble() override { m_provider->SetRumble(false); }
	std::string get_button_name(uint64 button) const override;
};
