/* Copyright (c) 2017 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <memory>

namespace Granite
{
class Variant
{
public:
	Variant() = default;

	template <typename T>
	explicit Variant(T &&t)
	{
		set(std::forward<T>(t));
	}

	template <typename T>
	void set(T &&t)
	{
		value = std::make_unique<HolderValue<T>>(std::forward<T>(t));
	}

	template <typename T>
	T &get()
	{
		return static_cast<HolderValue<T> *>(value.get())->value;
	}

	template <typename T>
	const T &get() const
	{
		return static_cast<const HolderValue<T> *>(value.get())->value;
	}

private:
	struct Holder
	{
		virtual ~Holder() = default;
	};

	template <typename U>
	struct HolderValue : Holder
	{
		template <typename P>
		HolderValue(P &&p)
		    : value(std::forward<P>(p))
		{
		}

		U value;
	};
	std::unique_ptr<Holder> value;
};
}