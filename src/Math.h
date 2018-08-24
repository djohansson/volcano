#pragma once

struct Vec4
{
	union
	{
		struct
		{
			float x;
			float y;
			float z;
			float w;
		};
		float data[4];
	};
};

template <typename T, typename U, typename V>
inline T clamp(T x, U lowerlimit, V upperlimit)
{
	return (x < lowerlimit ? lowerlimit : (x > upperlimit ? upperlimit : x));
}

template <typename T, typename U, typename V>
inline T ramp(T x, U edge0, V edge1)
{
	return (x - edge0) / (edge1 - edge0);
}

template <typename T>
inline T smoothstep(T x)
{
	return x * x * (3 - 2 * x);
}

template <typename T>
inline T smootherstep(T x)
{
	return x * x * x * (x * (x * 6 - 15) + 10);
}
