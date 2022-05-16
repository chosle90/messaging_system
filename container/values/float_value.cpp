#include "float_value.h"

#ifdef __USE_TYPE_CONTAINER__

#include "fmt/xchar.h"
#include "fmt/format.h"

namespace container
{
	float_value::float_value(void)
		: value()
	{
		_type = value_types::float_value;
	}

	float_value::float_value(const wstring& name, const float& value)
		: float_value()
	{
		_name = name;
		set_data((const unsigned char*)&value, sizeof(float), value_types::float_value);
	}

	float_value::~float_value(void)
	{
	}

	short float_value::to_short(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<short>(temp);
	}

	unsigned short float_value::to_ushort(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<unsigned short>(temp);
	}

	int float_value::to_int(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<int>(temp);
	}

	unsigned int float_value::to_uint(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<unsigned int>(temp);
	}

	long float_value::to_long(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<long>(temp);
	}

	unsigned long float_value::to_ulong(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<unsigned long>(temp);
	}

	long long float_value::to_llong(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<long long>(temp);
	}

	unsigned long long float_value::to_ullong(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<unsigned long long>(temp);
	}

	float float_value::to_float(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<float>(temp);
	}

	double float_value::to_double(void) const
	{
		float temp = 0;
		memcpy(&temp, _data.data(), _size);

		return static_cast<double>(temp);
	}

	wstring float_value::to_string(const bool&) const
	{
		return fmt::format(L"{}", to_float());
	}
}

#endif