export module piece_cannon;

import std;
import piece_base;

export class piece_cannon :public piece_base
{
public:
	piece_cannon() = default;
	~piece_cannon() = default;

	void create(const vulkan_application* _app, bool _use_color, bool _piece_color) override;

	glm::u8vec2 move_by(wchar_t _direction, uint8_t _move_number) override;
	std::pair<wchar_t, uint8_t> move_to(const glm::u8vec2& _new_location) override;

private:

};