#include "image_helper.h"

image_helper image_helper::helper;

image_helper& image_helper::get_image_help() noexcept
{
	return helper;
}
